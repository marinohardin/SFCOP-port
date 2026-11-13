#!/bin/sh
# The build's lift step: StarFleetOP.exe -> C, with pcrecomp's tools, into the build directory.
# CMake runs it (cmake/SfcopLifter.cmake); the lifted C is never committed.
#
#   lift.sh --exe FILE --sha256 HEX --pcrecomp DIR --out DIR --files N --marks VA[,VA...]
#
# Steps, all inside the sandbox container (no network, 4 CPUs, 6 GB; only the lift directory,
# the pcrecomp clone and this directory are mounted, the last two read-only):
#   rtti      pcrecomp tools/cpp/rtti.py        -> cat/rtti.json, cat/rtti_seeds.json
#   disasm    pcrecomp tools/disasm/disasm32.py -> cat/functions.json      (about 4 minutes)
#   lift      run_lift.py (with eh_frames.py)   -> out/gen/, out/lift_stats.json (about 2 minutes)
# The layout is the one scripts/progress.py --lifter reads.
#
# The lift is keyed by its inputs (the exe's hash, the pcrecomp revision, this directory's files
# and the patches applied to pcrecomp, the marks, the file count). When the key is unchanged
# nothing is rerun and no output is touched, so a fresh checkout of the same sources does not
# recompile the lifted code. When it changes, only the generated files whose content changed are
# replaced.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
PCRECOMP_REV=ef3f0da6e08a8eb7a3f869273f3df071ca8d7373

die() { echo "lift: $*" >&2; exit 1; }
sha256() { if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$@"; else sha256sum "$@"; fi; }
# The image is tagged by its Dockerfile's hash, so a changed Dockerfile builds a new image.
IMAGE=sfcop-lifter-py:$(sha256 "$HERE/Dockerfile" | cut -c1-12)

exe= want= pcrecomp= out= files= marks=
while [ $# -gt 0 ]; do
    case "$1" in
        --exe) exe=$2; shift 2 ;;
        --sha256) want=$2; shift 2 ;;
        --pcrecomp) pcrecomp=$2; shift 2 ;;
        --out) out=$2; shift 2 ;;
        --files) files=$2; shift 2 ;;
        --marks) marks=$2; shift 2 ;;
        *) die "unknown argument $1" ;;
    esac
done
[ -n "$exe" ] && [ -n "$want" ] && [ -n "$pcrecomp" ] && [ -n "$out" ] && [ -n "$files" ] \
    && [ -n "$marks" ] || die "usage: lift.sh --exe FILE --sha256 HEX --pcrecomp DIR --out DIR --files N --marks VAS"

[ -f "$exe" ] || die "no game executable at $exe (set SFCOP_INSTALL_DIR)"
got=$(sha256 "$exe" | cut -d' ' -f1)
[ "$got" = "$want" ] || die "$exe has sha256 $got, not the original's $want (analysis/results/retail_hashes.sha256)"

[ -f "$pcrecomp/tools/lift/lift32.py" ] || die "no pcrecomp clone at $pcrecomp (run cmake/lifter/setup.sh DIR, or set SFCOP_PCRECOMP_DIR)"
rev=$(git -C "$pcrecomp" rev-parse HEAD 2>/dev/null) || die "$pcrecomp is not a git clone"
[ "$rev" = "$PCRECOMP_REV" ] || die "pcrecomp at $pcrecomp is at $rev, not $PCRECOMP_REV"

command -v docker >/dev/null 2>&1 || die "docker is not installed; the lift runs pcrecomp's Python in a container"
docker info >/dev/null 2>&1 || die "docker is not running"

key=$( (echo "exe $got"; echo "pcrecomp $rev"; echo "files $files"; echo "marks $marks"
        sha256 "$HERE/run_lift.py" "$HERE/eh_frames.py" "$HERE/lift.sh" "$HERE/Dockerfile" \
            "$HERE"/patches/*.patch | cut -d' ' -f1) \
      | sha256 | cut -d' ' -f1)
mkdir -p "$out"
if [ -f "$out/key" ] && [ "$(cat "$out/key")" = "$key" ] && [ -f "$out/out/gen/recomp_dispatch.c" ]; then
    echo "lift: up to date ($out)"
    exit 0
fi

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "lift: building the $IMAGE image from cmake/lifter/Dockerfile"
    docker build -q -t "$IMAGE" "$HERE" >/dev/null
fi

rm -f "$out/key"
mkdir -p "$out/game" "$out/cat" "$out/out"
# The copy keeps the original's read-only mode, so a later lift removes it before copying again.
rm -f "$out/game/StarFleetOP.exe"
cp "$exe" "$out/game/StarFleetOP.exe"
run() {
    docker run --rm --network none --cpus 4 --memory 6g \
        -v "$out":/w -v "$pcrecomp":/p:ro -v "$HERE":/s:ro -e PCRECOMP_TOOLS=/p/tools -w /w \
        "$IMAGE" "$@"
}
step() {
    name=$1; shift
    start=$(date +%s)
    run "$@" > "$out/$name.log" 2>&1 || { tail -20 "$out/$name.log" >&2; die "$name failed; log: $out/$name.log"; }
    echo "lift: $name done in $(( $(date +%s) - start )) s"
}
step rtti python -I /p/tools/cpp/rtti.py game/StarFleetOP.exe -o cat/rtti.json --seeds cat/rtti_seeds.json
step disasm python -I /p/tools/disasm/disasm32.py game/StarFleetOP.exe -o cat/functions.json \
    --seed-functions cat/rtti_seeds.json
rm -rf "$out/out/gen.new"
step lift python -I /s/run_lift.py --all --out out/gen.new --files "$files" --mark "$marks"
grep -A4 '^====' "$out/lift.log" | sed 's/^/lift: /' || true

# Replace only what changed, so that an identical lift recompiles nothing.
mkdir -p "$out/out/gen"
for f in "$out/out/gen"/*; do       # files an earlier lift wrote and this one does not
    if [ -e "$f" ] && [ ! -e "$out/out/gen.new/$(basename "$f")" ]; then rm "$f"; fi
done
changed=0
for f in "$out/out/gen.new"/*; do
    dest="$out/out/gen/$(basename "$f")"
    if cmp -s "$f" "$dest"; then rm "$f"; else mv "$f" "$dest"; changed=$((changed + 1)); fi
done
rmdir "$out/out/gen.new"
rm -f "$out/game/StarFleetOP.exe"
echo "$key" > "$out/key"
echo "lift: $changed generated file(s) changed"
