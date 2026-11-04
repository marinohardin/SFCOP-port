#!/bin/sh
# run_all.sh: reproduce the guest-mode spike's results (C3, DESIGN §12.4). From the repository root:
#
#   sh spike/guest-mode/run_all.sh
#
# Steps (STEPS="..." runs a subset; default all):
#   manifest  tools/manifest.py --check and its tests
#   harness   the three claimed functions scored in VC6 mode (harness/build-and-verify.sh)
#   build     two native builds with spike/guest-mode/guest.cmake as the project include:
#             all-lifted (build/macos-arm64, SFCOP_GUEST_SOURCES=OFF) and guest
#             ($SFCOP_WORK/C3/build-guest, SFCOP_GUEST_SOURCES=ON); targets sfcop, sfcop_guest_test
#   test      sfcop_guest_test in both; their "[test]" lines must be identical
#   boot      tools/boot_probe.sh, three headless boots of each sfcop with the clock at 15:38
#             (a boot that reaches the CD-key prompt); the guest build's "[override]" lines say which
#             claimed functions the boot ran and how often
#
# Results go to spike/guest-mode/results/. The manifest and harness steps need no build; test and
# boot need both builds. The install folder is read from the all-lifted build's
# SFCOP_INSTALL_DIR (the lift finds it, or configure with -DSFCOP_INSTALL_DIR=...). Every boot
# runs on a fresh APFS clone of it. Builds run at -j4 under nice.
set -eu

root=$(pwd)
[ -f "$root/game/port/types.h" ] || { echo "run_all.sh: run it from the repository root" >&2; exit 2; }
work=${SFCOP_WORK:-$SFCOP_ROOT/work}
lifted=$root/build/macos-arm64
guest=$work/C3/build-guest
results=$root/spike/guest-mode/results
steps=${STEPS:-manifest harness build test boot}
mkdir -p "$results" "$work/C3"

has() { case " $steps " in *" $1 "*) return 0 ;; esac; return 1; }

if has manifest; then
    python3 tools/manifest.py --check
    python3 -m pytest -q tests/test_manifest.py
fi

if has harness; then
    # VC6 mode: each claimed function scored against the original's bytes by the harness.
    # A function that fails to build or is not 100.00% exact fails the step, after every one is scored.
    failed=0
    for f in 0x006fc4d0 0x006fc5a0 0x006fc750; do
        rc=0
        nice -n 19 sh harness/build-and-verify.sh "$f" > "$work/C3/harness_$f.txt" 2>&1 || rc=$?
        echo "== $f"
        grep -E '^(OK|FAIL|ERROR|RESULT)' "$work/C3/harness_$f.txt" || cat "$work/C3/harness_$f.txt"
        if [ "$rc" -ne 0 ] || ! grep -q "^OK    $f .* 100\.00% exact" "$work/C3/harness_$f.txt"; then
            echo "run_all.sh: $f is not exact (harness exit $rc)" >&2
            failed=1
        fi
    done > "$results/harness_vc6.txt"
    cat "$results/harness_vc6.txt"
    [ "$failed" -eq 0 ] || exit 1
fi

if has build; then
    cmake --preset macos-arm64 -DCMAKE_PROJECT_INCLUDE="$root/spike/guest-mode/guest.cmake" \
        -DSFCOP_GUEST_SOURCES=OFF > "$work/C3/configure.log"
    cmake --preset macos-arm64 -B "$guest" -DCMAKE_PROJECT_INCLUDE="$root/spike/guest-mode/guest.cmake" \
        -DSFCOP_GUEST_SOURCES=ON > "$work/C3/configure-guest.log"
    grep "guest-mode spike" "$work/C3/configure-guest.log"
    nice -n 19 cmake --build "$lifted" -j4 --target sfcop sfcop_guest_test > "$work/C3/build.log"
    nice -n 19 cmake --build "$guest" -j4 --target sfcop sfcop_guest_test > "$work/C3/build-guest.log"
fi

# The test and boot steps need both builds (the build step, now or earlier) and the install folder.
if has test || has boot; then
    for dir in "$lifted" "$guest"; do
        [ -x "$dir/sfcop" ] && [ -x "$dir/sfcop_guest_test" ] ||
            { echo "run_all.sh: no build in $dir; run the build step first" >&2; exit 2; }
    done
    install=$(sed -n 's/^SFCOP_INSTALL_DIR:PATH=//p' "$lifted/CMakeCache.txt")
    [ -f "$install/StarFleetOP.exe" ] || { echo "run_all.sh: no StarFleetOP.exe in the install folder" >&2; exit 2; }
fi

if has test; then
    status=0
    "$lifted/sfcop_guest_test" "$install/StarFleetOP.exe" > "$results/guest_test_lifted.txt" || status=1
    "$guest/sfcop_guest_test" "$install/StarFleetOP.exe" > "$results/guest_test_guest.txt" || status=1
    grep '^\[test\]' "$results/guest_test_lifted.txt" > "$work/C3/test_lines_lifted.txt"
    grep '^\[test\]' "$results/guest_test_guest.txt" > "$work/C3/test_lines_guest.txt"
    if cmp -s "$work/C3/test_lines_lifted.txt" "$work/C3/test_lines_guest.txt"; then
        echo "guest test: the [test] lines of both builds are identical" | tee "$results/guest_test_compare.txt"
    else
        diff "$work/C3/test_lines_lifted.txt" "$work/C3/test_lines_guest.txt" > "$results/guest_test_compare.txt" || true
        echo "guest test: the [test] lines differ (results/guest_test_compare.txt)" >&2
        status=1
    fi
    tail -n 1 "$results/guest_test_lifted.txt" "$results/guest_test_guest.txt"
    # The direct path: where each binary's claimed symbols live, and what the lifted
    # mGetAllocatorID(const char*) (sub_006FCC10) calls. In the guest build its `bl` targets are the
    # override entry points (low addresses, the sfcop_guest objects), not lifted bodies.
    for build in lifted guest; do
        eval dir=\$$build
        echo "== $build build: the claimed symbols"
        nm -m "$dir/sfcop" | grep -E '_sub_006FC(4D0|5A0|750)$'
        echo "== $build build: calls made by the lifted sub_006FCC10"
        objdump -d --no-show-raw-insn --disassemble-symbols=_sub_006FCC10 "$dir/sfcop" | grep -E '[[:space:]]bl[[:space:]]'
    done > "$results/direct_calls.txt"
    cat "$results/direct_calls.txt"
    [ "$status" -eq 0 ] || exit 1
fi

if has boot; then
    # 1793749080 is 15:38 on the day of the boot platform's probes (src/app/boot/results/README.md).
    for build in lifted guest; do
        eval dir=\$$build
        rm -rf "$work/C3/boot_$build"
        tools/boot_probe.sh -s "$install" -w 12 -t 1793749080 -l "$work/C3/boot_$build" \
            -o "$results/boot_1538_$build.tsv" 3 -- \
            "$dir/sfcop" --game @GAME@ --headless --watchdog @WATCHDOG@
        cat "$results/boot_1538_$build.tsv"
    done
    # What the overrides did in the guest boots: the announcement, each first run, the call counts.
    for i in 1 2 3; do
        echo "== run $i"
        grep -h '^\[override\]' "$work/C3/boot_guest/run_$i.out" "$work/C3/boot_guest/run_$i.err" || true
    done > "$results/boot_1538_guest_overrides.txt"
    cat "$results/boot_1538_guest_overrides.txt"
fi
