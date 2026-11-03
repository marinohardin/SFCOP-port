#!/bin/sh
# One-time (idempotent) setup of the oracle's Wine prefix:
#   1. initialise a wow64 prefix under work/wine/prefix (Gcenx Wine 11.0_1, see wine-env.sh)
#   2. copy the untouched 2.5.5.2 game from <private>/ into C:\Games\SFCOP (never run it in place)
#   3. apply the settings recorded in BASELINE.md: fullscreen game inside a Wine virtual desktop, renderer
#
# Options (environment): ORACLE_DESKTOP=800x600 (empty to disable), ORACLE_RENDERER=gl|vulkan|no3d|"" (default: Wine's own)
set -eu

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
. "$here/wine-env.sh"
game_src="$repo/<install>"

[ -x "$WINE_APP/Contents/Resources/wine/bin/wine" ] || {
    echo "setup_prefix.sh: Wine not found at $WINE_APP; see README.md (Install Wine)" >&2
    exit 2
}

if [ ! -f "$WINEPREFIX/system.reg" ]; then
    mkdir -p "$WINEPREFIX"
    wineboot -i
    wineserver -w
fi

# Verify the source before copying (hashes from analysis/results/retail_hashes.sha256).
expected=$(grep 'Starfleet Command Orion Pirates/StarFleetOP.exe$' \
    "$repo/analysis/results/retail_hashes.sha256" | cut -d' ' -f1 | head -1)
actual=$(shasum -a 256 "$game_src/StarFleetOP.exe" | cut -d' ' -f1)
if [ -n "$expected" ] && [ "$expected" != "$actual" ]; then
    echo "setup_prefix.sh: StarFleetOP.exe hash mismatch ($actual, expected $expected)" >&2
    exit 1
fi

mkdir -p "$GAME_DIR"
# --ignore-existing keeps the prefix copy's own edits (sfc.ini, installed .scr) on rerun.
rsync -a --ignore-existing "$game_src/" "$GAME_DIR/"
chmod -R u+w "$GAME_DIR"

# Fullscreen in the copy's sfc.ini (CRLF preserved): inside the Wine virtual desktop this is the
# only mode that presents; windowed=1 renders but shows black (BASELINE.md).
perl -pi -e 's/^windowed=1/windowed=0/' "$GAME_DIR/sfc.ini"

desktop=${ORACLE_DESKTOP-800x600}
if [ -n "$desktop" ]; then
    wine reg add 'HKCU\Software\Wine\Explorer' /v Desktop /d Default /f
    wine reg add 'HKCU\Software\Wine\Explorer\Desktops' /v Default /d "$desktop" /f
else
    wine reg delete 'HKCU\Software\Wine\Explorer' /v Desktop /f 2>/dev/null || true
fi
if [ -n "${ORACLE_RENDERER:-}" ]; then
    wine reg add 'HKCU\Software\Wine\Direct3D' /v renderer /d "$ORACLE_RENDERER" /f
fi
wineserver -w

# The CD key, if export_cdkey.sh saved one (work/private, never in the repo).
private=${SFCOP_PRIVATE:-$SFCOP_ROOT/work/private}
if [ -f "$private/cdkey.reg" ] || [ -f "$private/cdkey-won.reg" ] || [ -e "$private/sfcop.env" ]; then
    "$here/import_cdkey.sh" || echo "setup_prefix.sh: no CD key imported; the game will ask for it" >&2
fi

echo "prefix: $WINEPREFIX"
echo "game:   $GAME_DIR"
shasum -a 256 "$GAME_DIR/StarFleetOP.exe"
