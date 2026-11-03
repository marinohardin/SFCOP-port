#!/bin/sh
# Reproduce P1-2: the lifted StarFleetOP.exe on win32hle with the HLE gaps
# filled. See ../README.md.
#   spike/lifter/hle/scripts/run_all.sh                 # everything
#   STEPS="build selftest" run_all.sh                   # a subset (names below)
# Needs the P0-4 spike's output in work/lifter (spike/lifter/scripts/run_all.sh).
# Nothing here runs third-party Python; the win32hle C is compiled into the host.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
HLE=$(dirname "$HERE")
SPIKE=$(dirname "$HLE")
REPO=$(cd "$SPIKE/../.." && pwd)
WORK=$(cd "$REPO/.." && pwd)/work
case "$REPO" in */.claude/worktrees/*) WORK=$(cd "$REPO/../../../.." && pwd)/work;; esac
SRC=${SRC:-$WORK/lifter}                  # the spike's tree (read only)
LIFTER=${LIFTER:-$WORK/lifter-hle}        # ours
RES="$HLE/results"
PCRECOMP_REV=ef3f0da6e08a8eb7a3f869273f3df071ca8d7373
STEPS=${STEPS:-"setup build selftest run plain"}
WATCHDOG=${WATCHDOG:-60}
has() { case " $STEPS " in *" $1 "*) return 0;; esac; return 1; }
mkdir -p "$LIFTER" "$RES"
cd "$LIFTER"

if has setup; then       # APFS clones of the spike's tree; the patches applied to our pcrecomp copy
  for d in pcrecomp game cat gameroot; do [ -e "$d" ] || cp -Rc "$SRC/$d" "$d"; done
  mkdir -p out
  [ -e out/gen ] || cp -Rc "$SRC/out/gen" out/gen
  [ "$(git -C pcrecomp rev-parse HEAD)" = "$PCRECOMP_REV" ] || { echo "pcrecomp is not at $PCRECOMP_REV"; exit 1; }
  for p in "$SPIKE/patches/pcrecomp-tib-hook.patch" "$HLE"/patches/pcrecomp-*.patch; do
    if git -C pcrecomp apply --check "$p" 2>/dev/null; then git -C pcrecomp apply "$p"; echo "applied $(basename "$p")"
    elif git -C pcrecomp apply --reverse --check "$p" 2>/dev/null; then echo "already applied $(basename "$p")"
    else echo "does not apply: $p"; exit 1; fi
  done
fi
if has build; then       # first time ~5 min at -j4 (the lifted C); after that seconds
  cmake -S "$HLE/host" -B build -G Ninja -DLIFTER="$LIFTER" > build.log 2>&1
  /usr/bin/time -p cmake --build build -j 4 >> build.log 2>&1
  grep -v 'Building C object' build.log > "$RES/build.txt"
  ls -l build/sfcop build/hle_selftest
fi
if has selftest; then
  mkdir -p selftest
  SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=dummy ./build/hle_selftest selftest/scratch > selftest/out.txt 2>&1 \
    && rc=0 || rc=$?
  cp selftest/out.txt "$RES/selftest.txt"
  tail -1 selftest/out.txt
  [ "$rc" = 0 ]
fi
if has run; then         # headless, traced, to the watchdog: how far the boot gets
  mkdir -p runs shots/headless
  ./build/sfcop --game gameroot --run --headless --trace --watchdog "$WATCHDOG" --shots shots/headless --shot-every 5 \
    > runs/headless.out 2> runs/headless.err || true
  grep -v '^\[hle\]' runs/headless.err > "$RES/run_headless.txt"; cat runs/headless.out >> "$RES/run_headless.txt"
  grep '^\[hle\]' runs/headless.err | awk '{print $2}' | sort | uniq -c | sort -rn > "$RES/run_headless_import_counts.txt"
  grep -E '^\[(throw|wait|hle-todo|module|mark)\]|^=== |ICALL' "$RES/run_headless.txt" || true
fi
if has plain; then       # headless as above but untraced: the boot at full speed
  mkdir -p runs shots/plain
  ./build/sfcop --game gameroot --run --headless --watchdog "$WATCHDOG" --shots shots/plain \
    > runs/plain.out 2> runs/plain.err || true
  cat runs/plain.err runs/plain.out > "$RES/run_headless_untraced.txt"
  grep -E '^\[(throw|wait|module|mark)\]|^=== ' "$RES/run_headless_untraced.txt" || true
fi
if has window; then      # a window on this Mac, silent: the frames as shown, and the frame rate
  mkdir -p runs shots/window
  ./build/sfcop --game gameroot --run --silent --watchdog "$WATCHDOG" --shots shots/window --shot-every 5 \
    > runs/window.out 2> runs/window.err || true
  grep -E '^\[(pe|mark|ddraw|d3d|module|fps|shot|screen|mss|throw|wait|guest)\]|^=== ' runs/window.err > "$RES/run_window.txt" || true
  tail -3 "$RES/run_window.txt"
fi
