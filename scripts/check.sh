#!/bin/sh
# The one command that is the definition of done. CI runs the same steps, and an agent that
# says "done" has run this once at the end (DESIGN §7.4). In order:
#
#   1. the Python tests (tests/)
#   2. the hotspot list and CODEOWNERS agree
#   3. the override table src/app/guest/override.cpp is what the decompilation's annotations give
#   4. the C++ tests (every CTest test, after building the executables they run), when the
#      native build is configured here
#   5. the replay oracle, when a recorded trace, the native client and a prepared data folder
#      exist (src/app/oracle/README.md)
#
# The decompilation is ${SFCOP_DECOMP_DIR:-decomp} (the submodule, decomp/docs/INTERFACE.md); its
# own gates (the PORT: lines in its source, the progress and matched-set gates) are its
# repository's check, not this one's.
#
# A step whose inputs are absent on this machine prints `skip: <why>` and does not fail; it is
# never left to pass silently.
#
# Usage: sh scripts/check.sh

set -eu

cd "$(dirname "$0")/.."
decomp=${SFCOP_DECOMP_DIR:-$(pwd)/decomp}

skip() { echo "skip: $*"; }
tmp=$(mktemp -d "${TMPDIR:-/tmp}/sfcop-check.XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM

echo "== tests"
python3 -m pytest tests -q

echo "== hotspot list and CODEOWNERS agree"
python3 - <<'EOF'
import re, sys
pats = [l.strip() for l in open('.github/hotspots.txt') if l.strip() and not l.startswith('#')]
owners = [l.split()[0].lstrip('/').rstrip('/') for l in open('.github/CODEOWNERS') if l.strip() and not l.startswith('#')]
missing = [p for p in pats if p.replace('/**', '').rstrip('/') not in owners]
if missing:
    print("hotspots.txt patterns with no CODEOWNERS line:", missing); sys.exit(1)
print("ok:", len(pats), "hotspot patterns,", len(owners), "CODEOWNERS lines")
EOF

echo "== override table is current"
# src/app/guest/override.cpp is generated from the decompilation's annotations
# (decomp/docs/INTERFACE.md, "The guest table").
if [ ! -f "$decomp/INTERFACE_VERSION" ]; then
    skip "no decompilation at $decomp (git submodule update --init)"
else
    python3 "$decomp/tools/manifest.py" --only-override --override src/app/guest/override.cpp --check
    echo "ok: src/app/guest/override.cpp is what $decomp/tools/manifest.py writes"
fi

echo "== C++ tests"
# Every file a registered test runs is built first, asked of the folder itself
# (scripts/ctest_targets.py): a lift-on folder registers more tests than sfcop_tests, and CTest
# reports a test whose executable is absent as "Not Run". Nothing a test does not name is built.
if [ -f build/macos-arm64/CMakeCache.txt ]; then
    # The plan reads the folder's generated files, so they are brought up to date first: after a
    # change to a CMakeLists.txt they would otherwise name the tests of the configuration before.
    cmake --build --preset macos-arm64 --target build.ninja > "$tmp/regenerate.log" 2>&1 \
        || { tail -n 40 "$tmp/regenerate.log"; exit 1; }
    python3 scripts/ctest_targets.py plan build/macos-arm64 > "$tmp/test-targets" \
        || { cat "$tmp/test-targets"; exit 1; }
    targets=$(tr '\n' ' ' < "$tmp/test-targets")
    if [ -n "$targets" ]; then
        echo "building what the tests run: $targets(a lift-on folder's guest tests link the" \
             "lifted game: a first build lifts and compiles it, which takes minutes)"
        # shellcheck disable=SC2086
        cmake --build --preset macos-arm64 --target $targets > "$tmp/build-tests.log" 2>&1 \
            || { tail -n 40 "$tmp/build-tests.log"; exit 1; }
    fi
    python3 scripts/ctest_targets.py missing build/macos-arm64 || exit 1
    ctest --preset macos-arm64
else
    skip "the native build is not configured here (cmake --preset macos-arm64)"
fi

echo "== replay oracle"
# Each recorded trace is played on the native client with its steps file (src/app/oracle/): the
# client sets the mission up through the game's menus, waits for its END, writes the actual trace
# at <logs>/<mission>/<input>.trace and exits 0 (identical) or 4 (a tick differs); then every
# pair is compared again with --tree for one line each. The run needs a prepared per-user folder
# with a stored key (oracle/driver/README.md): its path comes from $SFCOP_ORACLE_DATA or the
# first line of the file $SFCOP_ORACLE_CONFIG names, is never printed, and nothing in it is read
# here; the mission's log leaves it through --logs.
traces=$(find oracle/expected -name '*.trace' 2>/dev/null | head -n 1)
oracle_data=${SFCOP_ORACLE_DATA:-}
if [ -z "$oracle_data" ]; then
    oracle_config=${SFCOP_ORACLE_CONFIG:-${XDG_CONFIG_HOME:-${HOME:-/nonexistent}/.config}/sfcop/oracle-data}
    if [ -f "$oracle_config" ]; then
        IFS= read -r oracle_data < "$oracle_config" || true
    fi
fi
oracle_watchdog=${SFCOP_ORACLE_WATCHDOG:-900}
if [ -z "$traces" ]; then
    skip "no recorded trace in oracle/expected/"
elif [ ! -x build/macos-arm64/sfcop ]; then
    skip "no native client (build/macos-arm64/sfcop)"
elif [ -z "$oracle_data" ] || [ ! -d "$oracle_data" ]; then
    skip "no prepared data folder with a stored key (set SFCOP_ORACLE_DATA, or put its path on the first line of \$SFCOP_ORACLE_CONFIG)"
else
    actual=$tmp/oracle
    for expected in oracle/expected/*/*.trace; do
        mission=$(basename "$(dirname "$expected")")
        input=$(basename "$expected" .trace)
        steps=oracle/driver/$input.steps
        if [ ! -f "$steps" ]; then
            echo "oracle: $expected has no steps file $steps"
            exit 2
        fi
        echo "oracle: $mission/$input on build/macos-arm64/sfcop, headless, up to $oracle_watchdog s"
        rc=0
        # fix_hle_low_memory_reads is off here: a guest read of 0x00010000-0x0001FFFF ends the
        # run, so the oracle shows such reads instead of continuing past them (docs/FIXES.md).
        nice -n 19 build/macos-arm64/sfcop --data "$oracle_data" --logs "$actual" --headless \
            --oracle "$expected" --input "$steps" --watchdog "$oracle_watchdog" \
            --fix=fix_hle_low_memory_reads=off \
            > "$tmp/oracle-$mission-$input.log" 2>&1 || rc=$?
        grep -E '^\[oracle\]|^\[mark\]|^sfcop:' "$tmp/oracle-$mission-$input.log" | sed 's/^/  /'
        case $rc in
        0 | 4) ;;
        *) echo "oracle: the client exited $rc"; exit "$rc" ;;
        esac
    done
    rc=0
    python3 oracle/trace/compare.py --tree oracle/expected "$actual" || rc=$?
    [ "$rc" = 0 ] || exit "$rc"
fi

echo "check: ok"
