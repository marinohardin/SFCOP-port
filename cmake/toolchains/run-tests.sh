#!/bin/sh
# Runs sfcop_tests of a cross build the way CI does (cmake/toolchains/README.md, "Tests"):
#
#   sh cmake/toolchains/run-tests.sh <preset> <build folder>
#
# 1. Every test case except the preset's lines in expected-failures.tsv, through the preset's
#    emulator. This run must pass; it decides the exit status.
# 2. The expected failures, each named with its reason, run once more for the record. Their result
#    never changes the exit status; one that passes now is reported, so that its line is removed.
#
# `ctest --preset <preset>` runs everything and shows the expected failures as failures.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
[ $# -eq 2 ] || { echo "usage: run-tests.sh <preset> <build folder>" >&2; exit 2; }
preset=$1
build=$(cd "$2" && pwd)
case $preset in
    windows-x64) run="sh $here/run-windows.sh"; tests=$build/sfcop_tests.exe ;;
    linux-x64) run="sh $here/run-linux.sh"; tests=$build/sfcop_tests ;;
    linux-arm64)
        . "$here/pins.env"
        run="env SFCOP_RUN_PLATFORM=linux/arm64 SFCOP_RUN_IMAGE=$LINUX_ARM64_RUN_IMAGE sh $here/run-linux.sh"
        tests=$build/sfcop_tests ;;
    *) echo "run-tests.sh: no cross preset $preset" >&2; exit 2 ;;
esac
[ -f "$tests" ] || { echo "run-tests.sh: $tests is not built" >&2; exit 1; }

# doctest filters are comma-separated with * and ? wildcards: a comma inside a name becomes ?.
filter=""
count=0
while IFS="$(printf '\t')" read -r p name entry why; do
    [ "$p" = "$preset" ] || continue
    echo "expected to fail on $preset ($entry, README.md \"Known failures\"): $name -- $why"
    filter="$filter${filter:+,}$(printf '%s' "$name" | tr ',' '?')"
    count=$((count + 1))
done < "$here/expected-failures.tsv"

cd "$build"
if [ "$count" -eq 0 ]; then
    $run "$tests" --no-version
    exit $?
fi
echo "== $preset: every test case but the $count expected to fail"
$run "$tests" --no-version "--test-case-exclude=$filter"
echo "== $preset: the $count expected to fail (for the record; the exit status is the run above)"
if $run "$tests" --no-version "--test-case=$filter" --reporters=console; then
    echo "note: every expected failure on $preset passes now: remove its lines from expected-failures.tsv"
fi
exit 0
