#!/bin/sh
# The native build's object-lift step (cmake/lifter/objlift.cmake runs it; DESIGN §6.3, §7.2):
#
#   objlift.sh --manifest TSV --exe FILE --gen DIR --pcrecomp DIR --out DIR
#
#   1. compile   every unit the manifest claims, with VC6 in the toolchain container, exactly as the
#                harness does (tools/objlift/compile.py) -> DIR/obj/<unit>.obj and .asm
#   2. lift      tools/objlift/objlift.py in the lifter container (cmake/lifter/Dockerfile: the
#                exe lift's image, with capstone), against the exe lift in --gen -> DIR/objlift_*
#
# The decompilation (its op/ sources, harness/, toolchain/ and ghidra/exports) is
# ${SFCOP_DECOMP_DIR:-<repository>/decomp} (decomp/docs/INTERFACE.md).
#
# The lifter container runs with no network, the repository, the decompilation, the exe, the exe
# lift and pcrecomp read-only, and only DIR writable. compile.py recompiles a unit only when its
# source or a header changed, and objlift.py rewrites a file only when its content changed, so an
# unchanged unit recompiles nothing downstream. --lift-only skips step 1 (objects already in
# DIR/obj); OBJLIFT_PYTHON=<python with capstone> runs step 2 without the container.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
DECOMP=$(cd "${SFCOP_DECOMP_DIR:-$ROOT/decomp}" && pwd) || { echo "objlift: no decompilation at ${SFCOP_DECOMP_DIR:-$ROOT/decomp} (git submodule update --init)" >&2; exit 1; }
export SFCOP_DECOMP_DIR="$DECOMP"

die() { echo "objlift: $*" >&2; exit 1; }
sha256() { if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$@"; else sha256sum "$@"; fi; }

manifest= exe= gen= pcrecomp= out= lift_only=
while [ $# -gt 0 ]; do
    case "$1" in
        --manifest) manifest=$2; shift 2 ;;
        --exe) exe=$2; shift 2 ;;
        --gen) gen=$2; shift 2 ;;
        --pcrecomp) pcrecomp=$2; shift 2 ;;
        --out) out=$2; shift 2 ;;
        --lift-only) lift_only=1; shift ;;
        *) die "unknown argument $1" ;;
    esac
done
[ -n "$manifest" ] && [ -n "$exe" ] && [ -n "$gen" ] && [ -n "$pcrecomp" ] && [ -n "$out" ] \
    || die "usage: objlift.sh --manifest TSV --exe FILE --gen DIR --pcrecomp DIR --out DIR"
mkdir -p "$out/obj"
out=$(cd "$out" && pwd)

start=$(date +%s)
if [ -z "$lift_only" ]; then
    "${PYTHON:-python3}" -I "$HERE/compile.py" --manifest "$manifest" --out "$out/obj" > "$out/obj/units.txt" \
        || die "a claimed unit does not compile (above)"
fi
echo "objlift: compile done in $(( $(date +%s) - start )) s"

# One --obj per unit, as the lift sees the path.
if [ -n "${OBJLIFT_PYTHON:-}" ]; then
    objroot=$out/obj
else
    objroot=/o/obj
fi
set --
while IFS= read -r line; do
    unit=${line%%=*}
    set -- "$@" --obj "$unit=$objroot/${unit%.*}.obj"
done < "$out/obj/units.txt"

start=$(date +%s)
if [ -n "${OBJLIFT_PYTHON:-}" ]; then
    "$OBJLIFT_PYTHON" "$HERE/objlift.py" "$@" --manifest "$manifest" --symbols "$DECOMP/ghidra/exports" \
        --exe "$exe" --lifted "$gen" --pcrecomp "$pcrecomp" --out "$out" \
        || die "the lift failed (above)"
else
    command -v docker >/dev/null 2>&1 || die "docker is not installed; the lift runs pcrecomp's Python in a container"
    image=sfcop-lifter-py:$(sha256 "$ROOT/cmake/lifter/Dockerfile" | cut -c1-12)
    if ! docker image inspect "$image" >/dev/null 2>&1; then
        echo "objlift: building the $image image from cmake/lifter/Dockerfile"
        docker build -q -t "$image" "$ROOT/cmake/lifter" >/dev/null
    fi
    mrel=$(cd "$(dirname "$manifest")" && pwd)/$(basename "$manifest")
    case "$mrel" in
        "$DECOMP"/*) mrel=/d/${mrel#"$DECOMP"/} ;;
        "$ROOT"/*) mrel=/r/${mrel#"$ROOT"/} ;;
        *) die "the manifest must be inside the decompilation ($DECOMP) or the repository ($ROOT)" ;;
    esac
    docker run --rm --network none --cpus 4 --memory 2g \
        -v "$ROOT":/r:ro -v "$DECOMP":/d:ro -e SFCOP_DECOMP_DIR=/d \
        -v "$exe":/x/StarFleetOP.exe:ro -v "$gen":/g:ro -v "$pcrecomp":/p:ro \
        -v "$out":/o -w /o "$image" \
        python -I /r/tools/objlift/objlift.py "$@" --manifest "$mrel" --symbols /d/ghidra/exports \
        --game /d/op --exe /x/StarFleetOP.exe --lifted /g --pcrecomp /p --out /o \
        || die "the lift failed (above)"
fi
echo "objlift: lift done in $(( $(date +%s) - start )) s"
touch "$out/objlift.stamp"
