#!/bin/sh
# Put the CD key saved by export_cdkey.sh back into the prefix's registry (32-bit view), e.g. after
# setup_prefix.sh made a fresh prefix. setup_prefix.sh calls this when a private file exists.
#
#   oracle/import_cdkey.sh
#
# Reads $SFCOP_PRIVATE (default work/private)/cdkey.reg and cdkey-won.reg with "reg import".
# The game stores the key Blowfish-encrypted (README.md, "CD key"), so a plaintext key in
# work/private/sfcop.env cannot be written directly: this script does not read that file. Enter the
# key once in the game (launch_game.sh), then run export_cdkey.sh.
#
# The key is private: never print, log or copy the files' contents. Do not add tracing (set -x).
set +x
set -eu

here=$(cd "$(dirname "$0")" && pwd)
. "$here/wine-env.sh"
private=${SFCOP_PRIVATE:-$SFCOP_ROOT/work/private}

imported=0
for file in "$private/cdkey.reg" "$private/cdkey-won.reg"; do
    [ -f "$file" ] || continue
    wine reg import "Z:$file" /reg:32 >/dev/null 2>&1 || {
        echo "import_cdkey.sh: reg import of $(basename "$file") failed" >&2
        exit 1
    }
    echo "imported $(basename "$file")"
    imported=1
done
wineserver -w

if [ "$imported" = 0 ]; then
    if [ -e "$private/sfcop.env" ]; then
        echo "import_cdkey.sh: only $private/sfcop.env exists. A plaintext key cannot be imported" >&2
        echo "  (the game stores it encrypted). Enter it once with oracle/launch_game.sh, then run" >&2
        echo "  oracle/export_cdkey.sh; later prefixes import that export." >&2
    else
        echo "import_cdkey.sh: no saved key in $private (run oracle/export_cdkey.sh after entering it once)" >&2
    fi
    exit 1
fi
