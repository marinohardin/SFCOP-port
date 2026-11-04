#!/bin/sh
# Builds the guest test program and lifts it, for the guest test host (../README.md, "Tests").
#
#   lift_ehtest.sh OUT PCRECOMP
#
#   OUT/root/ehtest.exe    ehtest.cpp, compiled by VC6 SP4 (toolchain/: /MT /GX /O2)
#   OUT/out/gen/           the lifted C, as cmake/lifter/lift.sh lifts the game: pcrecomp's rtti and
#                          disasm32, then run_lift.py --all, in the same container image
#
# The build's guest test target (sfcop_guest_tests) appears once OUT/out/gen exists: configure
# again after this. Without it, configuring says "sfcop_ehtest: skip".
# SFCOP_ROOT is set from the checkout's place when the environment does not.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
LIFTER="$REPO/cmake/lifter"
# toolchain/bin/msvc-run finds the VC6 libraries under SFCOP_ROOT, the parent of the repository
# (of the main checkout, from a worktree under .claude/worktrees/).
if [ -z "${SFCOP_ROOT:-}" ]; then
    root=$REPO
    case $root in */.claude/worktrees/*) root=${root%/.claude/worktrees/*} ;; esac
    SFCOP_ROOT=$(dirname "$root")
    export SFCOP_ROOT
fi
out=${1:?usage: lift_ehtest.sh OUT PCRECOMP}
pcrecomp=${2:?usage: lift_ehtest.sh OUT PCRECOMP}

sha256() { if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$@"; else sha256sum "$@"; fi; }
IMAGE=sfcop-lifter-py:$(sha256 "$LIFTER/Dockerfile" | cut -c1-12)

mkdir -p "$out/root" "$out/cat" "$out/out"
out=$(cd "$out" && pwd)
cp "$HERE/ehtest.cpp" "$out/"
(cd "$out" && "$REPO/toolchain/bin/cl" /nologo /c /MT /GX /O2 ehtest.cpp \
    && "$REPO/toolchain/bin/link" /nologo /subsystem:console /out:ehtest.exe ehtest.obj LIBCMT.LIB KERNEL32.LIB)
cp "$out/ehtest.exe" "$out/root/"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    docker build -q -t "$IMAGE" "$LIFTER" >/dev/null
fi
run() {
    docker run --rm --network none --cpus 4 --memory 6g \
        -v "$out":/w -v "$pcrecomp":/p:ro -v "$LIFTER":/s:ro -e PCRECOMP_TOOLS=/p/tools -w /w \
        "$IMAGE" "$@"
}
run python -I /p/tools/cpp/rtti.py ehtest.exe -o cat/rtti.json --seeds cat/rtti_seeds.json > "$out/rtti.log" 2>&1
run python -I /p/tools/disasm/disasm32.py ehtest.exe -o cat/functions.json \
    --seed-functions cat/rtti_seeds.json > "$out/disasm.log" 2>&1
rm -rf "$out/out/gen"
run python -I /s/run_lift.py --all --exe ehtest.exe --catalog cat/functions.json \
    --seeds cat/rtti_seeds.json --out out/gen > "$out/lift.log" 2>&1
grep -A4 '^====' "$out/lift.log" || tail -4 "$out/lift.log"
