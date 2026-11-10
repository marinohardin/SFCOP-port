#!/bin/sh
# windows-x64's CMAKE_CROSSCOMPILING_EMULATOR: runs a Windows x86_64 program from the Mac under
# the machine's Wine, in a prefix of its own.
#
#   sh cmake/toolchains/run-windows.sh <program.exe> [arguments...]
#
#   SFCOP_WINE_BIN       the folder holding wine and wineserver (default: the Wine the oracle uses,
#                        <tools>/wine/stable-11.0_1/Wine Stable.app/Contents/Resources/wine/bin)
#   SFCOP_CROSS_WINEPREFIX  the prefix (default: wine-prefix beside the program); created on first
#                        use. Never the oracle's prefix: this one holds nothing but Wine itself.
set -eu

[ $# -ge 1 ] || { echo "usage: run-windows.sh <program.exe> [arguments...]" >&2; exit 2; }
tools=${SFCOP_TOOLS_DIR:-}
[ -n "$tools" ] || [ -n "${SFCOP_WINE_BIN:-}" ] ||
    { echo "run-windows.sh: set SFCOP_TOOLS_DIR to the folder setup.sh installed into (or SFCOP_WINE_BIN)" >&2; exit 2; }
wine_bin=${SFCOP_WINE_BIN:-$tools/wine/stable-11.0_1/Wine Stable.app/Contents/Resources/wine/bin}
[ -x "$wine_bin/wine" ] || { echo "run-windows.sh: no Wine at $wine_bin (set SFCOP_WINE_BIN)" >&2; exit 1; }
prog_dir=$(cd "$(dirname "$1")" && pwd)
prog=$prog_dir/$(basename "$1")
shift

PATH="$wine_bin:$PATH"
WINEPREFIX=${SFCOP_CROSS_WINEPREFIX:-$prog_dir/wine-prefix}
# No Mono or Gecko install prompts, no debug chatter, no desktop integration.
WINEDLLOVERRIDES=${WINEDLLOVERRIDES:-mscoree=;mshtml=;winemenubuilder.exe=}
WINEDEBUG=${WINEDEBUG:--all}
export PATH WINEPREFIX WINEDLLOVERRIDES WINEDEBUG

if [ ! -f "$WINEPREFIX/system.reg" ]; then
    # One creator at a time: ctest may start several tests at once.
    lock=$WINEPREFIX.lock
    until mkdir "$lock" 2>/dev/null; do sleep 1; done
    trap 'rmdir "$lock" 2>/dev/null || true' EXIT
    if [ ! -f "$WINEPREFIX/system.reg" ]; then
        echo "run-windows.sh: creating the Wine prefix $WINEPREFIX" >&2
        wine wineboot --init >/dev/null 2>&1
        wineserver -w
    fi
    rmdir "$lock"
    trap - EXIT
fi
exec wine "$prog" "$@"
