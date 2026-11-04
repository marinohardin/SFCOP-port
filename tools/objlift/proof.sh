#!/bin/sh
# proof.sh: the object lift's proof (tools/objlift/README.md, "Proof"). From the repository root:
#
#   sh tools/objlift/proof.sh BUILD_DIR OUT_DIR [-- EXTRA CMAKE ARGS]
#
# One build directory, configured with tools/objlift/proof.cmake, built twice:
#   before   SFCOP_OBJLIFT=OFF: every function is the exe lift's body
#   after    SFCOP_OBJLIFT=ON:  the claimed functions are their lifted VC6 objects
# For each: the spike's guest test (spike/guest-mode/guest_test.c), the claimed symbols and the
# calls the lifted sub_006FCC10 makes (nm, objdump), and three headless boots with the clock at
# 15:38 and a 60 s watchdog (tools/boot_probe.sh). The [test] lines of the two must be identical
# and every boot must reach the CD-key prompt. Builds run at -j4 under nice.
set -eu

[ $# -ge 2 ] || { echo "usage: proof.sh BUILD_DIR OUT_DIR [-- CMAKE ARGS]" >&2; exit 2; }
root=$(pwd)
[ -f "$root/tools/objlift/proof.cmake" ] || { echo "proof.sh: run it from the repository root" >&2; exit 2; }
build=$1 out=$2
shift 2
[ "${1:-}" = "--" ] && shift
mkdir -p "$out"
status=0
# The lifted objects' announcement line is printed only with this set; the proof records it.
SFCOP_GUEST_VERBOSE=1
export SFCOP_GUEST_VERBOSE

if [ ! -f "$build/CMakeCache.txt" ]; then
    cmake -S "$root" -B "$build" --preset macos-arm64 -DSFCOP_LIFT=ON -DSFCOP_OBJLIFT=OFF \
        -DCMAKE_PROJECT_INCLUDE="$root/tools/objlift/proof.cmake" > "$out/configure.log"
fi

for stage in before after; do
    case $stage in before) objlift=OFF ;; after) objlift=ON ;; esac
    cmake -S "$root" -B "$build" -DSFCOP_OBJLIFT=$objlift "$@" > "$out/configure_$stage.log"
    grep -E '^-- sfcop: (SFCOP_OBJLIFT|lifting the VC6)' "$out/configure_$stage.log" || true
    t0=$(date +%s)
    nice -n 19 cmake --build "$build" -j4 --target sfcop sfcop_guest_test > "$out/build_$stage.log"
    echo "$stage: built in $(( $(date +%s) - t0 )) s"
    install=$(sed -n 's/^SFCOP_INSTALL_DIR:PATH=//p' "$build/CMakeCache.txt")

    "$build/sfcop_guest_test" "$install/StarFleetOP.exe" > "$out/guest_test_$stage.txt" 2>&1 || status=1
    grep '^\[test\]' "$out/guest_test_$stage.txt" > "$out/test_lines_$stage.txt"
    tail -n 1 "$out/guest_test_$stage.txt"

    {
        echo "== $stage: the claimed symbols"
        nm -m "$build/sfcop" | grep -E '_sub_006FC(4D0|5A0|750)$'
        echo "== $stage: calls made by the lifted sub_006FCC10"
        objdump -d --no-show-raw-insn --disassemble-symbols=_sub_006FCC10 "$build/sfcop" |
            grep -E '[[:space:]]bl[[:space:]]'
    } > "$out/direct_calls_$stage.txt"

    # 1793749080 is 15:38 on the day of the boot platform's probes (src/app/boot/results/README.md).
    rm -rf "$out/boot_$stage"
    tools/boot_probe.sh -s "$install" -w 60 -t 1793749080 -l "$out/boot_$stage" \
        -o "$out/boot_1538_$stage.tsv" 3 -- "$build/sfcop" --game @GAME@ --headless --watchdog @WATCHDOG@
    cat "$out/boot_1538_$stage.tsv"
    awk -F'\t' 'NR > 1 && $3 != "prompt" { bad = 1 } END { exit bad }' "$out/boot_1538_$stage.tsv" || status=1
done

if cmp -s "$out/test_lines_before.txt" "$out/test_lines_after.txt"; then
    echo "guest test: the [test] lines of both builds are identical" | tee "$out/guest_test_compare.txt"
else
    diff "$out/test_lines_before.txt" "$out/test_lines_after.txt" > "$out/guest_test_compare.txt" || true
    echo "guest test: the [test] lines differ ($out/guest_test_compare.txt)" >&2
    status=1
fi
# What the lifted objects did in the boots: the announcement, each first run, the call counts.
for i in 1 2 3; do
    echo "== run $i"
    grep -h -E '^\[(override|objlift)\]' "$out/boot_after/run_$i.out" "$out/boot_after/run_$i.err" || true
done > "$out/boot_1538_after_overrides.txt"
cat "$out/boot_1538_after_overrides.txt"
exit $status
