#!/bin/sh
# Plays a steps file into the native client: by default menu.steps, windowed, 90 s watchdog, which
# boots to the main menu (the Setup mark) and clicks SINGLE PLAYER.
#
#   oracle/driver/run_native.sh [--windowed|--fullscreen|--headless] [--steps FILE]
#                               [--watchdog S] [-- MORE SFCOP FLAGS]
#
# The data folder holds a stored key, so its path comes from configuration and is never printed,
# and nothing in it is read here:
#   SFCOP_ORACLE_DATA      the prepared data folder; else the first line of the file named by
#   SFCOP_ORACLE_CONFIG    (default: ${XDG_CONFIG_HOME:-$HOME/.config}/sfcop/oracle-data)
# Optional: SFCOP_BIN (default build/macos-arm64/sfcop), SFCOP_INSTALL (passed as --install).
#
# Exit: the client's (0 the watchdog saw its goal mark, 1 not, 2 bad input or a wait in the script
# ran out); 2 here when the configuration is missing.
set +x
set -u

root=$(cd "$(dirname "$0")/../.." && pwd)
mode=--windowed
steps="$root/oracle/driver/menu.steps"
# 90 s, not 30, until the boot time is fixed: the boot to the menu takes 12 to 23 s and the
# script runs about 3 s more. Lower it then.
watchdog=90
while [ $# -gt 0 ]; do
    case "$1" in
        --windowed|--fullscreen|--headless) mode=$1; shift ;;
        --steps) steps=$2; shift 2 ;;
        --watchdog) watchdog=$2; shift 2 ;;
        --) shift; break ;;
        *) echo "run_native: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

data=${SFCOP_ORACLE_DATA:-}
if [ -z "$data" ]; then
    config=${SFCOP_ORACLE_CONFIG:-${XDG_CONFIG_HOME:-$HOME/.config}/sfcop/oracle-data}
    if [ -f "$config" ]; then
        IFS= read -r data < "$config" || true
    fi
fi
if [ -z "$data" ] || [ ! -d "$data" ]; then
    echo "run_native: no prepared data folder: set SFCOP_ORACLE_DATA, or put its path on the first" \
         "line of \$SFCOP_ORACLE_CONFIG (default ~/.config/sfcop/oracle-data)" >&2
    exit 2
fi

bin=${SFCOP_BIN:-$root/build/macos-arm64/sfcop}
if [ ! -x "$bin" ]; then
    echo "run_native: no client at SFCOP_BIN ($bin); build it with the lift first" >&2
    exit 2
fi

# fix_hle_low_memory_reads is off here: a guest read of 0x00010000-0x0001FFFF ends the run, so the
# oracle shows such reads instead of continuing past them (docs/FIXES.md).
if [ -n "${SFCOP_INSTALL:-}" ]; then
    exec "$bin" --install "$SFCOP_INSTALL" --data "$data" "$mode" --input "$steps" \
        --watchdog "$watchdog" --fix=fix_hle_low_memory_reads=off "$@"
fi
exec "$bin" --data "$data" "$mode" --input "$steps" --watchdog "$watchdog" \
    --fix=fix_hle_low_memory_reads=off "$@"
