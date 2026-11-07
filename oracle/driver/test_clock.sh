#!/bin/sh
# Tests oracle_clock.dll and oracle_launch.exe under Wine, without the game: oracle_clock_check.exe
# reads the clock as the game does, through the launcher at the default step, and its output must
# be oracle_clock_check.expected (values written by hand from the rules in oracle_clock.c).
#
#   oracle/driver/test_clock.sh [build-out-dir]
#
# The programs come from build.sh's output (default work/oracle/build/out). The test runs in a
# prefix of its own, ORACLE_CLOCK_PREFIX (default work/oracle/clock-prefix), made on first use;
# the game's prefix is not touched. Exit 0 on a match, 1 on a difference (shown), 2 when a program
# is missing.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
. "$here/../wine-env.sh"
out=${1:-$SFCOP_ROOT/work/oracle/build/out}
export WINEPREFIX="${ORACLE_CLOCK_PREFIX:-$SFCOP_ROOT/work/oracle/clock-prefix}"
for f in oracle_launch.exe oracle_clock.dll oracle_clock_check.exe; do
    [ -f "$out/$f" ] || { echo "test_clock.sh: missing $out/$f (run build.sh)" >&2; exit 2; }
done
if [ ! -d "$WINEPREFIX" ]; then
    wineboot -i > /dev/null 2>&1
    wineserver -w
fi
actual=$(cd "$out" && wine oracle_launch.exe oracle_clock_check.exe 2>/dev/null | tr -d '\r')
wineserver -w
if [ "$actual" = "$(cat "$here/oracle_clock_check.expected")" ]; then
    echo "test_clock.sh: ok"
    exit 0
fi
echo "$actual" | diff "$here/oracle_clock_check.expected" - || true
exit 1
