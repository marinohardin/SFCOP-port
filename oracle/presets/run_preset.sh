#!/bin/sh
# Runs the oracle on one preset's build and writes its trace.
#
#   oracle/presets/run_preset.sh PRESET [--build DIR] [--root DIR] [--steps FILE]
#                                [--mission NAME --input NAME] [--watchdog S]
#
# PRESET is macos-arm64 (the client runs natively), linux-x64 (in the pinned container,
# cmake/toolchains/run-linux.sh) or windows-x64 (under Wine, run-windows.sh). The trace goes to
# ROOT/PRESET/MISSION/INPUT.trace (ROOT defaults to build/oracle-traces) with the build line
# `native PRESET COMMIT`.
#
# Two kinds of run, by MISSION:
#   MenuPath (the default; steps oracle/driver/menu.steps, input name `menu`): the client boots to
#     the main menu and clicks SINGLE PLAYER. The trace is the marks the game reached, in order
#     (marks_trace.py); their times go beside it in INPUT.timings, unhashed.
#   OracleSkirmish (input `skirmish`): the oracle mission plays and the trace is made from the
#     mission's log with oracle/trace/mklog.py. The client writes the log under the folder its
#     `--logs DIR` flag names and only OracleTrace.txt there is read; a client without that flag
#     skips the run (exit 3).
#
# The client is BUILD/sfcop (BUILD/sfcop.exe for windows-x64); BUILD defaults to
# build/PRESET, or $SFCOP_BUILD_ROOT/PRESET. Its install is $SFCOP_INSTALL or BUILD/gameroot.
# The data folder holds a stored key, so its path comes from configuration and is never printed,
# and nothing in it is read here except that it is copied, whole, to a scratch folder
# (BUILD/oracle-scratch/PRESET, removed at the end) which the client uses instead: the original is
# never written to, and nothing of it reaches ROOT:
#   SFCOP_ORACLE_DATA      the prepared data folder; else the first line of the file named by
#   SFCOP_ORACLE_CONFIG    (default: ${XDG_CONFIG_HOME:-$HOME/.config}/sfcop/oracle-data)
# The mission module: the skirmish needs the oracle mission translated for this preset's host ABI
# (tools/scr2native/scr2native.py on that preset's toolchain; src/app/scripts/ModuleContract.md §2).
# --modules DIR, else $SFCOP_ORACLE_MODULES/PRESET, is the client's modules root for the run
# (SFCOP_MISSION_MODULES); DIR/OracleSkirmish/module.json must exist, or the skirmish skips with that
# reason. Another preset's modules are never used. The menu path loads no module of its own.
# Niceness: the client runs at nice 19; for the Linux container the wrapper $BUILD/nice-exec.sh
# applies it inside (run-linux.sh passes the program to the container as it is), under Wine the
# host's wine processes inherit it. Docker on a Mac runs the container in a VM, so inside it the
# niceness orders only the VM's own processes.
# SFCOP_ORACLE_EMULATOR replaces the preset's way of running (a command that takes the client and
# its arguments); the shell tests use it with a stand-in client.
#
# Exit: 0 the trace is written; 1 the client failed or left no usable trace; 2 bad arguments or
# configuration; 3 skipped, with the reason on standard output ("skip: ...").
set +x
set -u

here=$(cd "$(dirname "$0")" && pwd)
root_dir=$(cd "$here/../.." && pwd)

preset=${1:-}
[ -n "$preset" ] || { echo "run_preset: usage: run_preset.sh PRESET [options]" >&2; exit 2; }
shift
case "$preset" in
    macos-arm64|linux-x64|windows-x64) ;;
    *) echo "run_preset: unknown preset '$preset'" >&2; exit 2 ;;
esac

build=${SFCOP_BUILD_ROOT:+$SFCOP_BUILD_ROOT/$preset}
build=${build:-$root_dir/build/$preset}
out_root=$root_dir/build/oracle-traces
steps=$root_dir/oracle/driver/menu.steps
mission=MenuPath
input=menu
watchdog=
modules=${SFCOP_ORACLE_MODULES:+$SFCOP_ORACLE_MODULES/$preset}
while [ $# -gt 0 ]; do
    case "$1" in
        --build) build=$2; shift 2 ;;
        --root) out_root=$2; shift 2 ;;
        --steps) steps=$2; shift 2 ;;
        --mission) mission=$2; shift 2 ;;
        --input) input=$2; shift 2 ;;
        --watchdog) watchdog=$2; shift 2 ;;
        --modules) modules=$2; shift 2 ;;
        *) echo "run_preset: unknown argument '$1'" >&2; exit 2 ;;
    esac
done
case "$mission:$input" in
    MenuPath:menu|OracleSkirmish:skirmish) ;;
    *) echo "run_preset: mission/input must be MenuPath/menu or OracleSkirmish/skirmish" >&2; exit 2 ;;
esac
[ "$mission" = OracleSkirmish ] && [ "$steps" = "$root_dir/oracle/driver/menu.steps" ] \
    && steps=$root_dir/oracle/driver/skirmish.steps
[ -n "$watchdog" ] || { [ "$mission" = OracleSkirmish ] && watchdog=900 || watchdog=60; }
[ -f "$steps" ] || { echo "run_preset: no steps file $steps" >&2; exit 2; }

exe=sfcop
[ "$preset" = windows-x64 ] && exe=sfcop.exe
client=$build/$exe
if [ ! -x "$client" ] && [ ! -f "$client" ]; then
    case "$preset" in
        windows-x64)
            echo "skip: $preset: no lifted client at $client; the lift does not build for windows-x64 yet" \
                 "(cmake/toolchains/README.md, 'The Windows driver'), so the game cannot run there" ;;
        *) echo "skip: $preset: no lifted client at $client (the build ran without the lift)" ;;
    esac
    exit 3
fi

data=${SFCOP_ORACLE_DATA:-}
if [ -z "$data" ]; then
    config=${SFCOP_ORACLE_CONFIG:-${XDG_CONFIG_HOME:-$HOME/.config}/sfcop/oracle-data}
    if [ -f "$config" ]; then
        IFS= read -r data < "$config" || true
    fi
fi
if [ -z "$data" ] || [ ! -d "$data" ]; then
    echo "skip: $preset: no prepared data folder (SFCOP_ORACLE_DATA, or the first line of \$SFCOP_ORACLE_CONFIG)"
    exit 3
fi

if [ "$mission" = OracleSkirmish ] && { [ -z "$modules" ] || [ ! -f "$modules/OracleSkirmish/module.json" ]; }; then
    echo "skip: $preset: no OracleSkirmish module for this preset's host ABI (SFCOP_ORACLE_MODULES/$preset/OracleSkirmish/module.json); translate it with tools/scr2native/scr2native.py on this preset's toolchain"
    exit 3
fi

install=${SFCOP_INSTALL:-$build/gameroot}
[ -d "$install" ] || { echo "run_preset: no install folder at $install (SFCOP_INSTALL)" >&2; exit 2; }

case "$preset" in
    linux-x64) emulator="sh $root_dir/cmake/toolchains/run-linux.sh" ;;
    windows-x64) emulator="sh $root_dir/cmake/toolchains/run-windows.sh" ;;
    *) emulator= ;;
esac
emulator=${SFCOP_ORACLE_EMULATOR:-$emulator}
# The program the emulator runs, and the environment the client needs inside it.
prog=$client
if [ -n "$modules" ]; then
    SFCOP_MISSION_MODULES=$modules
    export SFCOP_MISSION_MODULES
fi
if [ "$preset" = linux-x64 ] && [ -z "${SFCOP_ORACLE_EMULATOR:-}" ]; then
    # run-linux.sh passes only the program to the container: the wrapper (in the build folder, which
    # the container mounts) applies the niceness and the modules root inside it.
    printf '#!/bin/sh\n%s\nexec nice -n 19 "$@"\n' \
        "${modules:+export SFCOP_MISSION_MODULES=$modules}" > "$build/nice-exec.sh"
    chmod +x "$build/nice-exec.sh"
    prog="$build/nice-exec.sh $client"
    if [ -n "$modules" ]; then
        SFCOP_RUN_MOUNTS=${SFCOP_RUN_MOUNTS:+$SFCOP_RUN_MOUNTS:}$modules
        export SFCOP_RUN_MOUNTS
    fi
fi

scratch=$build/oracle-scratch/$preset
rm -rf "$scratch"
mkdir -p "$scratch" || exit 2
trap 'rm -rf "$scratch"' EXIT INT TERM
cp -R "$data" "$scratch/data" || { echo "run_preset: cannot prepare the data folder" >&2; exit 2; }

commit=$(git -C "$root_dir" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
out_dir=$out_root/$preset/$mission
mkdir -p "$out_dir" || exit 2
rm -f "$out_dir/$input.trace" "$out_dir/$input.timings"

logs_arg=
log_file=
if [ "$mission" = OracleSkirmish ]; then
    # The client writes the mission's log under the folder `--logs DIR` names (A2 adds the flag; this
    # grep matches the string `--logs` in the client's --help). Without it the log would land in the
    # data folder, which is not read, so the skirmish skips.
    # shellcheck disable=SC2086
    if (cd "$scratch" && $emulator $prog --help 2>&1) | grep -q -- '--logs'; then
        mkdir -p "$scratch/logs"
        logs_arg="--logs $scratch/logs"
        log_file=$scratch/logs/OracleTrace.txt
    else
        echo "skip: $preset: this client has no --logs flag yet, so the mission's log cannot be taken (A2 adds it)"
        exit 3
    fi
fi

echo "run_preset: $preset: $mission/$input"
rc=0
# cwd is the scratch folder: the Linux container mounts it writable and the data copy is in it.
# fix_hle_low_memory_reads is off here: a guest read of 0x00010000-0x0001FFFF ends the run, so the
# oracle shows such reads instead of continuing past them (docs/FIXES.md).
# shellcheck disable=SC2086
(cd "$scratch" && nice -n 19 $emulator $prog --install "$install" --data "$scratch/data" \
    --headless --input "$steps" --watchdog "$watchdog" --fix=fix_hle_low_memory_reads=off $logs_arg) \
    > "$scratch/stdout.log" 2> "$scratch/stderr.log" || rc=$?
if [ "$rc" -ne 0 ]; then
    echo "run_preset: $preset: the client exited $rc; last lines of its error output:" >&2
    # The client's error output may reach the CI log: lines naming the prepared data folder are dropped.
    tail -n 5 "$scratch/stderr.log" | grep -vF -e "$data" -e "$scratch" >&2
    exit 1
fi

exe_sha=$(shasum -a 256 "$client" | cut -d' ' -f1)
build_line="native $preset $commit"
case "$mission" in
    MenuPath)
        python3 -I "$here/marks_trace.py" "$scratch/stderr.log" --mission "$mission" --input "$input" \
            --exe-sha256 "$exe_sha" --build "$build_line" -o "$out_dir/$input.trace" \
            --timings "$out_dir/$input.timings" || exit 1 ;;
    OracleSkirmish)
        python3 -I "$root_dir/oracle/trace/mklog.py" "$log_file" --mission "$mission" --input "$input" \
            --exe-sha256 "$exe_sha" --build "$build_line" -o "$out_dir/$input.trace" || exit 1 ;;
esac
echo "run_preset: $preset: wrote $out_dir/$input.trace"
