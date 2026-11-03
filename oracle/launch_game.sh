#!/bin/sh
# Start the game in the oracle prefix for a human: first-run CD key entry, menu calibration, or
# just playing. Quit from the game menu, or run: . oracle/wine-env.sh && wineserver -k
# After the game exits, a CD key it stored is saved with export_cdkey.sh if none was saved yet,
# so the key only ever has to be typed once (README.md, "CD key").
set +x
set -eu
here=$(cd "$(dirname "$0")" && pwd)
. "$here/wine-env.sh"
private=${SFCOP_PRIVATE:-$SFCOP_ROOT/work/private}
cd "$GAME_DIR"
wine StarFleetOP.exe || true
wineserver -w
if [ ! -f "$private/cdkey.reg" ] && [ ! -f "$private/cdkey-won.reg" ]; then
    "$here/export_cdkey.sh" || true
fi
