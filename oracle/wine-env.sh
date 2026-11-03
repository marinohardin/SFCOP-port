# Source this file: sets up the Wine environment used by every oracle script.
# Wine: Gcenx macOS_Wine_builds 11.0_1 (wine-stable, wow64), unpacked on the external drive.
SFCOP_ROOT="${SFCOP_ROOT:-$SFCOP_ROOT}"
export WINE_APP="$SFCOP_ROOT/tools/wine/stable-11.0_1/Wine Stable.app"
export PATH="$WINE_APP/Contents/Resources/wine/bin:$PATH"
export WINEPREFIX="${WINEPREFIX:-$SFCOP_ROOT/work/wine/prefix}"
export WINEDEBUG="${WINEDEBUG:--all}"
# Game folder inside the prefix: a copy of reference/installed game/..., never the reference itself.
export GAME_WIN='C:\Games\SFCOP'
export GAME_DIR="$WINEPREFIX/drive_c/Games/SFCOP"
# Quiet MoltenVK startup logging.
export MVK_CONFIG_LOG_LEVEL="${MVK_CONFIG_LOG_LEVEL:-0}"
