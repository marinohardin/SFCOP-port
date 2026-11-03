#!/bin/sh
# Boot StarFleetOP.exe in the oracle prefix with one graphics configuration, take screenshots of
# the Wine virtual desktop region, then kill Wine. Used to produce BASELINE.md.
#
#   boot_test.sh <label> [seconds=90] [shot-every=30]
# Environment: RENDERER=gl|vulkan|no3d (Direct3D renderer; empty = Wine default),
#              WINDOWED=0|1 (sfc.ini [3D] windowed, default 0), DESKTOP=800x600 (empty = no virtual desktop)
#
# Screenshots: oracle/shot.sh, the game's own macOS window only. (A fixed screen region was used
# first; it also records any other app on top, so it is gone.) The window has to be on the visible
# Space for macOS to composite its GL content; otherwise the shot is black.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
. "$here/../wine-env.sh"
label=$1
seconds=${2:-90}
every=${3:-30}
out=$here/$label
mkdir -p "$out"

wineserver -k 2>/dev/null || true
wineserver -w 2>/dev/null || true

if [ -n "${RENDERER:-}" ]; then
    wine reg add 'HKCU\Software\Wine\Direct3D' /v renderer /d "$RENDERER" /f >/dev/null 2>&1
else
    wine reg delete 'HKCU\Software\Wine\Direct3D' /v renderer /f >/dev/null 2>&1 || true
fi
if [ -n "${DESKTOP-800x600}" ]; then
    wine reg add 'HKCU\Software\Wine\Explorer' /v Desktop /d Default /f >/dev/null 2>&1
    wine reg add 'HKCU\Software\Wine\Explorer\Desktops' /v Default /d "${DESKTOP-800x600}" /f >/dev/null 2>&1
else
    wine reg delete 'HKCU\Software\Wine\Explorer' /v Desktop /f >/dev/null 2>&1 || true
fi
perl -pi -e "s/^windowed=[01]/windowed=${WINDOWED:-0}/" "$GAME_DIR/sfc.ini"
wineserver -w

{
    echo "label $label"
    echo "wine $(wine --version)"
    echo "renderer ${RENDERER:-default} windowed ${WINDOWED:-0} desktop ${DESKTOP-800x600}"
    echo "seconds $seconds"
} > "$out/config.txt"

( cd "$GAME_DIR" && WINEDEBUG=${WINEDEBUG_BOOT:-err+all,fixme-all} exec wine StarFleetOP.exe ) > "$out/wine.log" 2>&1 &

t=0
while [ "$t" -lt "$seconds" ]; do
    sleep "$every"
    t=$((t + every))
    "$here/../shot.sh" "$out/shot_${t}s.png" || true
done
if pgrep -f 'StarFleetOP.exe' >/dev/null; then echo "alive_at_end yes" >> "$out/config.txt"; else echo "alive_at_end no" >> "$out/config.txt"; fi

wineserver -k 2>/dev/null || true
wineserver -w 2>/dev/null || true
grep -E '^[0-9a-f]{4}:(err|warn)' "$out/wine.log" | sed -E 's/^[0-9a-f]{4}://' | sort | uniq -c | sort -rn | head -20 > "$out/errors.txt" || true
cat "$out/config.txt"
