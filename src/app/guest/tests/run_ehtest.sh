#!/bin/sh
# Runs the guest tests (../README.md, "Tests"): every case of ehtest.exe on the guest test host,
# then the uncaught throw. CTest runs it as sfcop_guest_tests.
#
#   run_ehtest.sh HOST ROOT SCRATCH
#
# Passes when all 22 cases pass with exit status 0, and on a POSIX host each of the 28 access
# violations (one each in two cases, two in a third, 20 in the fourth, four in the fifth) was
# handled on the main thread's alternate signal stack, on macOS each one's return from its handler
# into an older frame ran there too, and the uncaught throw is reported (its C++ type is int, ".H"), the C runtime's
# top-level filter ends it with "abnormal program termination", and the run exits with status 1.
set -u
cases=22  # PASS lines: one per case in ehtest.cpp's main
faults=28 # access violations: see the comment above
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

seen=$(grep -c '^\[eh\] access violation: ' "$scratch/run.err" || true)
onstack=$(grep -c '^\[eh\] access violation: .*, on the alternate stack$' "$scratch/run.err" || true)
echo "access violations: $seen, handled on the alternate stack: $onstack"
resumes=$(grep -c '^\[eh\] resuming after a fault' "$scratch/run.err" || true)
resumesOn=$(grep -c '^\[eh\] resuming after a fault, on the alternate stack$' "$scratch/run.err" || true)
echo "returns into an older frame: $resumes, on the alternate stack: $resumesOn"
case $(uname -s) in
    MINGW* | MSYS* | CYGWIN* | Windows*) onstack=$faults ;; # a Windows host has no signal stack
esac
resumesOk=1
case $(uname -s) in
    # Every fault's catch is in a frame older than the fault, so each returns into one once.
    Darwin) [ "$resumes" -eq "$faults" ] && [ "$resumesOn" -eq "$faults" ] || resumesOk=0 ;;
esac
passes=$(grep -c '^PASS' "$scratch/run.out" || true)
fails=$(grep -c '^FAIL' "$scratch/run.out" || true)
if [ "$rc" -ne 0 ] || [ "$fails" -ne 0 ] || [ "$passes" -ne "$cases" ] || [ "$seen" -ne "$faults" ] \
    || [ "$onstack" -ne "$faults" ] || [ "$resumesOk" -ne 1 ] || [ "$rc2" -ne 1 ] \
    || ! grep -q 'unhandled guest exception 0xE06D7363, C++ type .H ' "$scratch/uncaught.err" \
    || ! grep -q '^abnormal program termination' "$scratch/uncaught.err"; then
    echo "guest tests: FAILED"
    exit 1
fi
echo "guest tests: all $cases cases pass, every access violation handled on the alternate stack; the uncaught throw is reported and terminates the run"
