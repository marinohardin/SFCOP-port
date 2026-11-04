#!/bin/sh
# boot_proof.sh: the kernel and registry modules on the boot to the CD-key prompt (macOS).
#
#   src/hle/kernel/boot_proof.sh BUILD OUT
#
# BUILD is a lift build (it holds sfcop and gameroot/, the copy of the install the boot runs
# on); OUT is a new directory for the results. The script clones gameroot/ into OUT/game, notes
# every file's modification time, and runs the boot twice with the wall clock pinned to 15:38
# (the time of day src/app/boot/results/ shows reaching the prompt) and HOME set to OUT/home, so
# the per-user folder is OUT/home/Library/Application Support/SFCOP: once plain (exit 0 at the
# prompt) and once with --trace. Then it checks that nothing under OUT/game changed and lists
# what the game wrote in the per-user folder.
#
# Results in OUT: plain.err and trace.err (the boot's logs), kernel_rows.txt (the trace lines
# this module printed), game_before.txt / game_after.txt (path and modification time of every
# file), game_changed.txt (files modified after the run started; empty when the game folder was
# only read), user_files.txt (the per-user folder afterwards), summary.txt.
set -eu

[ $# -eq 2 ] || { echo "usage: src/hle/kernel/boot_proof.sh BUILD OUT" >&2; exit 2; }
build=$(cd "$1" && pwd)
out=$2
[ -x "$build/sfcop" ] && [ -d "$build/gameroot" ] || { echo "boot_proof: $build is not a lift build" >&2; exit 2; }
[ ! -e "$out" ] || { echo "boot_proof: $out exists" >&2; exit 2; }
mkdir -p "$out"
out=$(cd "$out" && pwd)
here=$(cd "$(dirname "$0")/../../.." && pwd)

cp -Rc "$build/gameroot" "$out/game"
chmod -R u+w "$out/game"
mkdir -p "$out/home"
cc -dynamiclib -o "$out/fakeclock.dylib" "$here/tools/fakeclock.c"

stamp() { (cd "$out/game" && find . -type f -exec stat -f '%m %N' {} + | sort -k2); }
stamp > "$out/game_before.txt"
touch "$out/started"
sleep 1

epoch=1793749080 # 15:38 on the day the boot results were recorded
run() {
    offset=$((epoch - $(date +%s)))
    rc=0
    HOME="$out/home" SFCOP_FAKE_CLOCK_OFFSET=$offset DYLD_INSERT_LIBRARIES="$out/fakeclock.dylib" \
        "$build/sfcop" --game "$out/game" --headless --watchdog 60 "$@" > /dev/null 2> "$err" || rc=$?
    echo "$rc"
}
err=$out/plain.err; plain=$(run)
err=$out/trace.err; traced=$(run --trace)

stamp > "$out/game_after.txt"
(cd "$out/game" && find . -newer "$out/started" -type f | sort) > "$out/game_changed.txt"
grep -a '^\[hle-kernel\]\|^\[hle-registry\]' "$out/trace.err" > "$out/kernel_rows.txt" || true
(cd "$out/home" && find . -type f | sort) > "$out/user_files.txt"

{
    echo "plain run exit: $plain (0: the CD-key prompt)"
    echo "plain run: $(grep -a '(CD-key prompt) at' "$out/plain.err" | head -1 || true)"
    echo "traced run exit: $traced"
    echo "trace lines from the kernel and registry modules: $(wc -l < "$out/kernel_rows.txt" | tr -d ' ')"
    echo "game files modified during the runs: $(wc -l < "$out/game_changed.txt" | tr -d ' ')"
    if cmp -s "$out/game_before.txt" "$out/game_after.txt"; then
        echo "game folder modification times: unchanged"
    else
        echo "game folder modification times: CHANGED"
    fi
    echo "files in the per-user folder: $(wc -l < "$out/user_files.txt" | tr -d ' ')"
} > "$out/summary.txt"
cat "$out/summary.txt"
