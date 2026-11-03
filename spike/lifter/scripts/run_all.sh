#!/bin/sh
# Reproduce the P0-4 lifter spike end to end. See ../README.md.
#   spike/lifter/scripts/run_all.sh            # everything
#   STEPS="lift build run" run_all.sh          # a subset (names below)
# Third-party Python (pcrecomp's tools) runs only inside the sandbox container
# (sandbox.sh: no network, work/lifter mounted). Our own scripts run on the host.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
SPIKE=$(dirname "$HERE")
REPO=$(cd "$SPIKE/../.." && pwd)
LIFTER=${LIFTER:-$(cd "$REPO/.." && pwd)/work/lifter}
export LIFTER PIP_CACHE_DIR=${PIP_CACHE_DIR:-$(dirname "$LIFTER")/pip-cache}
RES="$SPIKE/results"
GAME_REF="$REPO/reference/installed game/Starfleet Command Orion Pirates"
EXE_SHA=264da726075c30fea181273e29ac9c98a51936011a7fdf7a490bec4fe4d2c883
PCRECOMP_REV=ef3f0da6e08a8eb7a3f869273f3df071ca8d7373
STEPS=${STEPS:-"setup rtti disasm lift census build run"}
PY="$LIFTER/venv/bin/python"
SB="$HERE/sandbox.sh"
has() { case " $STEPS " in *" $1 "*) return 0;; esac; return 1; }
mkdir -p "$LIFTER" "$RES"
cd "$LIFTER"

if has setup; then
  [ -d pcrecomp ] || git clone -q https://github.com/sp00nznet/pcrecomp pcrecomp
  git -C pcrecomp checkout -q "$PCRECOMP_REV"
  git -C pcrecomp apply --check "$SPIKE/patches/pcrecomp-tib-hook.patch" 2>/dev/null \
    && git -C pcrecomp apply "$SPIKE/patches/pcrecomp-tib-hook.patch"
  mkdir -p game
  cp "$GAME_REF/StarFleetOP.exe" game/StarFleetOP.exe
  echo "$EXE_SHA  game/StarFleetOP.exe" | shasum -a 256 -c -
  [ -d gameroot ] || cp -Rc "$GAME_REF" gameroot       # APFS clone; the run writes here, never reference/
  [ -x venv/bin/python ] || python3 -m venv venv
  venv/bin/pip install -q capstone==5.0.7 pefile==2024.8.26
  docker build -q -t sfcop-lifter-py "$SPIKE/docker"
fi
if has rtti; then
  mkdir -p cat
  "$SB" python -I pcrecomp/tools/cpp/rtti.py game/StarFleetOP.exe -o cat/rtti.json --seeds cat/rtti_seeds.json \
    > cat/rtti.log 2>&1
  cp cat/rtti.log "$RES/"
fi
if has disasm; then     # ~4 min
  "$SB" python -I pcrecomp/tools/disasm/disasm32.py game/StarFleetOP.exe -o cat/functions.json \
    --seed-functions cat/rtti_seeds.json > cat/disasm.log 2>&1
  cp cat/disasm.log "$RES/"
fi
if has lift; then       # ~2 min, 191 MB of C in out/gen
  "$SB" python -I /s/run_lift.py --all --out out/gen > out/lift.log 2>&1
  cp out/lift.log out/lift_stats.json "$RES/"
fi
if has census; then     # our scripts, on the host
  "$PY" -I "$HERE/imports_vs_hle.py" game/StarFleetOP.exe pcrecomp/runtime/win32hle "$RES/imports_vs_win32hle.tsv"
  "$PY" -I "$HERE/census.py" game/StarFleetOP.exe cat/functions.json "$RES"
  "$PY" -I "$HERE/dx_inventory.py" "$RES/com_calls.tsv" pcrecomp/runtime/win32hle "$RES/dx_inventory.md" > /dev/null
fi
if has build; then      # ~6 min at -j4; needs Xcode CLT, cmake, ninja, pkg-config, sdl2 (Homebrew)
  cmake -S "$SPIKE/host" -B build -G Ninja -DLIFTER="$LIFTER" > build.log 2>&1
  /usr/bin/time -p cmake --build build -j 4 >> build.log 2>&1
  grep -v 'Building C object' build.log > "$RES/build.log"
  ls -l build/sfcop
fi
if has run; then        # dry run, then a headless run with a 60 s watchdog
  ./build/sfcop --game gameroot
  mkdir -p runs
  ./build/sfcop --game gameroot --run --headless --trace --watchdog 60 > runs/run3.out 2> runs/run3.err || true
  grep -v '^\[hle\]' runs/run3.err > "$RES/run3.err.txt"; cat runs/run3.out >> "$RES/run3.err.txt"
  grep '^\[hle\]' runs/run3.err | awk '{print $2}' | sort | uniq -c | sort -rn > "$RES/run3_import_counts.txt"
  n=$(grep -n '\[mark\]' runs/run3.err | cut -d: -f1)
  [ -n "$n" ] && tail -n +"$n" runs/run3.err | grep '^\[hle\]' | awk '{print $2}' \
    | grep -v 'CriticalSection\|GetCurrentThreadId\|TlsGetValue\|LastError\|Interlocked' | uniq -c \
    > "$RES/run3_imports_after_winmain.txt"
  cat "$RES/run3.err.txt"
fi
