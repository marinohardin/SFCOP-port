#!/bin/sh
# Reproduce task P1-1 (guest exceptions in the lifted build) end to end. See ../README.md.
#   spike/lifter/eh/scripts/run_eh.sh                 # everything (about 15 minutes)
#   STEPS="guest-test game-run" run_eh.sh             # a subset (names below)
# Works in its own copy of the spike's lifted tree, work/lifter-eh (never work/lifter).
# Third-party Python (pcrecomp's tools) runs only inside the sandbox container.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
EH=$(dirname "$HERE")
REPO=$(cd "$EH/../../.." && pwd)
SFCOP=${SFCOP_ROOT:-$SFCOP_ROOT}
SRC=${SRC_LIFTER:-$SFCOP/work/lifter}            # the P0-4 spike's tree (read only)
LIFTER=${LIFTER:-$SFCOP/work/lifter-eh}
export LIFTER SFCOP_ROOT=$SFCOP PIP_CACHE_DIR=${PIP_CACHE_DIR:-$SFCOP/work/pip-cache}
RES="$EH/results"
STEPS=${STEPS:-"setup guest guest-cat guest-lift guest-build guest-test frames game-lift game-build game-run"}
PY="$LIFTER/venv/bin/python"
SB="$HERE/sandbox.sh"
TC="$REPO/toolchain/bin"
has() { case " $STEPS " in *" $1 "*) return 0;; esac; return 1; }
mkdir -p "$RES"

if has setup; then
  # The spike's clone of pcrecomp (at ef3f0da, TIB patch applied), catalogue and exe.
  mkdir -p "$LIFTER"
  for d in pcrecomp cat game gameroot; do
    [ -e "$LIFTER/$d" ] || cp -Rc "$SRC/$d" "$LIFTER/$d"
  done
  patch="$EH/patches/pcrecomp-eh-landing-pads.patch"
  if git -C "$LIFTER/pcrecomp" apply --reverse --check "$patch" 2>/dev/null; then
    echo "[setup] landing-pad patch already applied"
  elif git -C "$LIFTER/pcrecomp" apply --check "$patch"; then
    git -C "$LIFTER/pcrecomp" apply "$patch"
  else
    echo "[setup] landing-pad patch does not apply to $LIFTER/pcrecomp: stopping" >&2
    exit 1
  fi
  git -C "$LIFTER/pcrecomp" diff --stat
  [ -x "$PY" ] || python3 -m venv "$LIFTER/venv"
  "$LIFTER/venv/bin/pip" install -q capstone==5.0.7 pefile==2024.8.26
fi
cd "$LIFTER"

if has guest; then      # VC6 SP4 under wibo (toolchain/); /GX, static multithreaded CRT
  mkdir -p ehtest/root
  cp "$EH/guest/ehtest.cpp" ehtest/
  (cd ehtest && "$TC/cl" /nologo /c /MT /GX /O2 ehtest.cpp \
     && "$TC/link" /nologo /subsystem:console /map:ehtest.map /out:ehtest.exe ehtest.obj LIBCMT.LIB KERNEL32.LIB)
  cp ehtest/ehtest.exe ehtest/root/
fi
if has guest-cat; then
  mkdir -p ehtest/cat
  "$SB" python -I pcrecomp/tools/cpp/rtti.py ehtest/ehtest.exe -o ehtest/cat/rtti.json \
    --seeds ehtest/cat/rtti_seeds.json > ehtest/cat/rtti.log 2>&1
  "$SB" python -I pcrecomp/tools/disasm/disasm32.py ehtest/ehtest.exe -o ehtest/cat/functions.json \
    --seed-functions ehtest/cat/rtti_seeds.json > ehtest/cat/disasm.log 2>&1
fi
if has guest-lift; then
  mkdir -p ehtest/out
  "$SB" python -I /s/run_lift_eh.py --all --exe ehtest/ehtest.exe --catalog ehtest/cat/functions.json \
    --seeds ehtest/cat/rtti_seeds.json --out ehtest/out/gen > ehtest/out/lift.log 2>&1
  tail -4 ehtest/out/lift.log
fi
if has guest-build; then
  cmake -S "$EH/host" -B build-ehtest -G Ninja -DLIFTER="$LIFTER" -DGEN_DIR="$LIFTER/ehtest/out/gen" > build-ehtest.log 2>&1
  cmake --build build-ehtest -j 4 >> build-ehtest.log 2>&1
fi
if has guest-test; then
  # All cases, then the uncaught throw (expected: reported, exit status 7).
  rc=0; ./build-ehtest/sfcop --game ehtest/root --exe ehtest.exe --run --headless --watchdog 60 \
    > ehtest/run.out 2> ehtest/run.err || rc=$?
  rc2=0; ./build-ehtest/sfcop --game ehtest/root --exe ehtest.exe --args uncaught --run --headless --watchdog 60 \
    > ehtest/uncaught.out 2> ehtest/uncaught.err || rc2=$?
  {
    echo "# ehtest: lifted VC6 test program on win32hle + eh/runtime (run_eh.sh guest-test)"
    grep -E '^(PASS|FAIL|ehtest:)' ehtest/run.out
    grep '^\[eh\] [0-9]' ehtest/run.err || true
    echo "exit status: $rc"
    echo
    echo "# ehtest uncaught"
    grep -E '^ehtest:' ehtest/uncaught.out || true
    grep '^\[eh\]' ehtest/uncaught.err || true
    echo "exit status: $rc2"
  } > "$RES/ehtest.txt"
  cat "$RES/ehtest.txt"
  fails=$(grep -c '^FAIL' ehtest/run.out || true)
  passes=$(grep -c '^PASS' ehtest/run.out || true)
  if [ "$rc" -ne 0 ] || [ "$fails" -ne 0 ] || [ "$passes" -ne 14 ] || [ "$rc2" -ne 7 ] \
     || ! grep -q 'unhandled guest exception 0xE06D7363, C++ type .H' ehtest/uncaught.err; then
    echo "guest-test: FAILED"; exit 1
  fi
  echo "guest-test: all 14 cases pass, the uncaught throw is reported"
fi
if has frames; then     # our analysis, on the host
  "$PY" -I "$HERE/eh_frames.py" game/StarFleetOP.exe cat/functions.json "$RES/eh_frames_game.json" \
    "$RES/eh_functions_game.tsv" | sed -n '/"cxx_funcinfo/,/"seh_finally/p'
fi
if has game-lift; then  # ~2 min
  mkdir -p out
  "$SB" python -I /s/run_lift_eh.py --all --mark 0x0069A840 --out out/gen > out/lift.log 2>&1
  tail -4 out/lift.log
  "$PY" -I -c 'import json,sys; s=json.load(open(sys.argv[1])); json.dump(s["eh"], open(sys.argv[2],"w"), indent=1)' \
    out/lift_stats.json "$RES/lift_eh_game.json"
fi
if has game-build; then # ~6 min at -j4
  cmake -S "$EH/host" -B build-game -G Ninja -DLIFTER="$LIFTER" -DGEN_DIR="$LIFTER/out/gen" > build-game.log 2>&1
  /usr/bin/time -p cmake --build build-game -j 4 >> build-game.log 2>&1
  tail -3 build-game.log
fi
if has game-run; then
  mkdir -p runs
  # 1. The spike's run-1 path: no OSSpecific DLL, so the game's fallback throws.
  SFCOP_EH_TRACE=1 ./build-game/sfcop --game gameroot --run --headless --watchdog 60 --no-osspecific \
    > runs/first-throw.out 2> runs/first-throw.err || true
  # 2. The spike's run 3 (OSSpecific answered), now with the runtime in place.
  SFCOP_EH_TRACE=1 ./build-game/sfcop --game gameroot --run --headless --watchdog 60 \
    > runs/boot.out 2> runs/boot.err || true
  # 3. Past run 3's stop: imports win32hle lacks fail instead of ending the run
  #    (test only), which drives the game into its own throw-and-catch paths.
  SFCOP_EH_TRACE=1 ./build-game/sfcop --game gameroot --run --headless --watchdog 60 --fail-missing \
    > runs/fail-missing.out 2> runs/fail-missing.err || true
  for r in first-throw boot fail-missing; do
    { grep -v '^\[hle\]' "runs/$r.err" | grep -v '^ICALL: unresolved' ; cat "runs/$r.out"; } > "$RES/game_$r.txt"
  done
  # Run 3 twice more: its exception sequence (the [eh] lines) must not change.
  same=0
  for k in 2 3; do
    SFCOP_EH_TRACE=1 ./build-game/sfcop --game gameroot --run --headless --watchdog 60 --fail-missing \
      > "runs/fail-missing-$k.out" 2> "runs/fail-missing-$k.err" || true
    grep '^\[eh\]' runs/fail-missing.err > runs/eh-1.txt || true
    grep '^\[eh\]' "runs/fail-missing-$k.err" > "runs/eh-$k.txt" || true
    if cmp -s runs/eh-1.txt "runs/eh-$k.txt"; then same=$((same + 1)); fi
  done
  echo "# runs 2 and 3: $same of 2 with the same [eh] lines as this run" >> "$RES/game_fail-missing.txt"
  for r in first-throw boot fail-missing; do
    echo "== $r"; grep '^\[eh\]\|\[mark\]\|^=== \|FATAL\|--fail-missing' "$RES/game_$r.txt" | cut -c1-160
  done
fi
