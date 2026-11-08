#!/bin/sh
# Runs the guest tests (../README.md, "Tests"): every case of ehtest.exe on the guest test host,
# then the uncaught throw. CTest runs it as sfcop_guest_tests.
#
#   run_ehtest.sh HOST ROOT SCRATCH
#
# Passes when all 17 cases pass with exit status 0, and the uncaught throw is reported (its C++
# type is int, ".H"), the C runtime's top-level filter ends it with "abnormal program termination",
# and the run exits with status 1.
set -u
host=${1:?usage: run_ehtest.sh HOST ROOT SCRATCH}
root=${2:?}
scratch=${3:?}
mkdir -p "$scratch/data"

rc=0
"$host" "$root" "$scratch/data" > "$scratch/run.out" 2> "$scratch/run.err" || rc=$?
rc2=0
"$host" "$root" "$scratch/data" uncaught > "$scratch/uncaught.out" 2> "$scratch/uncaught.err" || rc2=$?

grep -E '^(PASS|FAIL|ehtest:)' "$scratch/run.out"
echo "exit status: $rc"
grep -E '^ehtest:' "$scratch/uncaught.out" || true
grep -e '^\[eh\]' -e '^abnormal program termination' "$scratch/uncaught.err" || true
echo "uncaught: exit status $rc2"

passes=$(grep -c '^PASS' "$scratch/run.out" || true)
fails=$(grep -c '^FAIL' "$scratch/run.out" || true)
if [ "$rc" -ne 0 ] || [ "$fails" -ne 0 ] || [ "$passes" -ne 17 ] || [ "$rc2" -ne 1 ] \
    || ! grep -q 'unhandled guest exception 0xE06D7363, C++ type .H ' "$scratch/uncaught.err" \
    || ! grep -q '^abnormal program termination' "$scratch/uncaught.err"; then
    echo "guest tests: FAILED"
    exit 1
fi
echo "guest tests: all 17 cases pass; the uncaught throw is reported and terminates the run"
