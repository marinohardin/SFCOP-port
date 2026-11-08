#!/bin/sh
# The one command that is the definition of done. CI runs the same steps, and an agent that
# says "done" has run this once at the end (DESIGN §7.4). In order:
#
#   1. the Python tests (tests/, harness/tests/)
#   2. the hotspot list and CODEOWNERS agree
#   3. the PORT: lines in game/ (listed, never a failure) and the SFCOP_FIX sites there (a failure:
#      fixes to game code are override units under src/fixes/overrides/, DESIGN §6.9)
#   4. the progress gate: no function loses exact or functional status (scripts/progress.py)
#   5. the matched-set gate: no score drops below the base branch's (harness/build-and-verify.sh)
#   6. the C++ tests (sfcop_tests, CTest), when the native build is configured here
#   7. the replay oracle, when a recorded trace, the native client and a prepared data folder
#      exist (src/app/oracle/README.md)
#
# A step whose inputs are absent on this machine prints `skip: <why>` and does not fail; it is
# never left to pass silently. The base branch's copies are read with `git show <base>:<path>`;
# the base is $SFCOP_BASE_REF if set, else the base side of CI's merge checkout, else the merge
# base with origin/develop, else (on develop itself) the commit before.
#
# Usage: sh scripts/check.sh

set -eu

cd "$(dirname "$0")/.."

skip() { echo "skip: $*"; }
tmp=$(mktemp -d "${TMPDIR:-/tmp}/sfcop-check.XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM

echo "== tests"
python3 -m pytest tests harness/tests -q

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

echo "== PORT: lines in game/ (SFCOP_FIX sites there fail)"
if [ -d game ]; then
    python3 scripts/port_footprint.py --no-fix-sites game
else
    skip "no game/ tree yet"
fi

# The commit whose copies of the progress and matched-set files this tree is held to. Locally a
# branch is held to its merge base with origin/develop, not to origin/develop's tip: a figure
# develop raised after the branch was cut is not this branch's to keep, and CI (the merge with
# develop's tip) holds it to the tip anyway.
base_ref() {
    if [ -n "${SFCOP_BASE_REF:-}" ]; then
        echo "$SFCOP_BASE_REF"
    elif [ "${GITHUB_ACTIONS:-}" = true ] && git rev-parse --verify --quiet HEAD^2 >/dev/null; then
        git rev-parse HEAD^1                                    # CI: the merge's base side
    elif git rev-parse --verify --quiet origin/develop >/dev/null \
            && [ "$(git rev-parse HEAD)" != "$(git rev-parse origin/develop)" ]; then
        git merge-base HEAD origin/develop                     # a branch: where it left develop
    elif git rev-parse --verify --quiet HEAD^1 >/dev/null; then
        git rev-parse HEAD^1                                    # develop itself: the commit before
    fi
}
base=$(base_ref || true)

# The lift the progress figures are computed from: $SFCOP_LIFTED, the native build's, or the
# shared one progress.py defaults to ($SFCOP_WORK/lifter).
lift_dir() {
    work=${SFCOP_WORK:-$(dirname "$(dirname "$(git rev-parse --path-format=absolute --git-common-dir)")")/work}
    for d in ${SFCOP_LIFTED:-} build/macos-arm64/lifted "$work/lifter"; do
        if [ -f "$d/cat/functions.json" ] && [ -f "$d/out/lift_stats.json" ]; then
            echo "$d"
            return
        fi
    done
}

echo "== progress gate"
lifted=$(lift_dir || true)
if [ -z "$base" ]; then
    skip "no base commit to compare with (set SFCOP_BASE_REF)"
elif [ -z "$lifted" ]; then
    skip "no lifter output (build the native client, or set SFCOP_LIFTED)"
elif ! git show "$base:analysis/results/progress/progress.json" > "$tmp/progress-base.json" 2>/dev/null; then
    skip "$base has no analysis/results/progress/progress.json"
else
    echo "held to $base, lift in $lifted"
    rc=0
    python3 scripts/progress.py --gate "$tmp/progress-base.json" --lifter "$lifted" \
        --matched-set analysis/results/matched_set.json > "$tmp/progress.log" 2>&1 || rc=$?
    grep -E '^(Progress:|REGRESSION|gate:|progress:)' "$tmp/progress.log" || tail -n 20 "$tmp/progress.log"
    [ "$rc" = 0 ] || exit "$rc"
fi

echo "== matched set holds the base branch's"
have_install=$(python3 -c 'import os, sys; sys.path.insert(0, "."); from harness.lib import paths; print(1 if os.path.isfile(paths.EXE) else "")')
have_image=$(docker image inspect sfcop-msvc6:latest >/dev/null 2>&1 && echo 1 || true)
if [ -z "$base" ]; then
    skip "no base commit to compare with (set SFCOP_BASE_REF)"
elif [ -n "$have_install" ] && [ -n "$have_image" ]; then
    sh harness/build-and-verify.sh --all --base "$base"
else
    skip "no sfcop-msvc6 image or no install: the matched functions are not rebuilt; only their scores are held to $base"
    python3 - "$base" <<'EOF'
import sys
sys.path.insert(0, '.')
from harness.lib import matched, paths, verify
base, label = verify.load_base(sys.argv[1])
data = matched.load(paths.MATCHED_SET)
bad = 0
for k, old in sorted(base['functions'].items()):
    new = data['functions'].get(k)
    if matched.worse(new, old):
        what = 'drops' if new is None else 'lowers to %.2f%%' % new['score']
        print('REGRESSION %s %s: matched_set.json %s the entry in %s (%.2f%%%s)'
              % (k, old.get('name', ''), what, label, old['score'], ' exact' if old.get('exact') else ''))
        bad += 1
if bad:
    sys.exit(1)
print('ok: %d entries held to %s' % (len(base['functions']), label))
EOF
fi

echo "== C++ tests"
if [ -f build/macos-arm64/CMakeCache.txt ]; then
    cmake --build --preset macos-arm64 --target sfcop_tests > "$tmp/build-tests.log" 2>&1 \
        || { tail -n 40 "$tmp/build-tests.log"; exit 1; }
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
        nice -n 19 build/macos-arm64/sfcop --data "$oracle_data" --logs "$actual" --headless \
            --oracle "$expected" --input "$steps" --watchdog "$oracle_watchdog" \
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
