#!/bin/sh
# Save the CD key the game stored in the prefix's registry, so a fresh prefix never needs it typed
# again (import_cdkey.sh puts it back). Run once after the key was entered with launch_game.sh.
#
#   oracle/export_cdkey.sh
#
# Writes $SFCOP_PRIVATE (default work/private)/cdkey.reg for HKLM\SOFTWARE\GameSpy\CDKeys and
# cdkey-won.reg for HKLM\SOFTWARE\WON\CDKeys, whichever exist, mode 600. The game is 32-bit, so its
# HKLM keys live in the prefix's 32-bit registry view (/reg:32).
#
# The key is private: this script never prints, logs or copies registry data anywhere but those
# files. It prints only value names and types. Do not add tracing (set -x) or echo data here.
set +x
set -eu

here=$(cd "$(dirname "$0")" && pwd)
. "$here/wine-env.sh"
private=${SFCOP_PRIVATE:-$SFCOP_ROOT/work/private}
umask 077
mkdir -p "$private"

# Names and types of the values under a key, never the data.
names() {
    wine reg query "$1" /reg:32 2>/dev/null | tr -d '\r' |
        sed -n -E 's/^    (([^ ]| [^ ])+)  +(REG_[A-Z_]+).*$/    \1  \3/p'
}

found=0
for key in 'HKLM\SOFTWARE\GameSpy\CDKeys' 'HKLM\SOFTWARE\WON\CDKeys'; do
    case $key in
        *GameSpy*) file=$private/cdkey.reg ;;
        *) file=$private/cdkey-won.reg ;;
    esac
    wine reg query "$key" /reg:32 >/dev/null 2>&1 || continue
    rm -f "$file.tmp"
    wine reg export "$key" "Z:$file.tmp" /reg:32 /y >/dev/null 2>&1 || {
        rm -f "$file.tmp"
        echo "export_cdkey.sh: reg export of $key failed" >&2
        exit 1
    }
    chmod 600 "$file.tmp"
    mv -f "$file.tmp" "$file"
    echo "exported $key -> $file"
    names "$key"
    found=1
done
wineserver -w

if [ "$found" = 0 ]; then
    echo "export_cdkey.sh: no CD key in the prefix; enter it once with oracle/launch_game.sh" >&2
    exit 1
fi
