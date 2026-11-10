#!/bin/sh
# Wraps SFCOP.app in a disk image for a release (packaging/README.md):
#
#   sh packaging/macos/make_dmg.sh SFCOP.app OUT.dmg HASH_LIST
#
# The image holds a copy of the app and a link to /Applications, so the player drags one onto the
# other. It is compressed (UDZO), HFS+, and unsigned: signing and notarizing are the owner's. The
# finished image is mounted read-only and checked by packaging/check_package.py; a refused image
# is removed. SFCOP_PYTHON names the Python; python3 on PATH otherwise.
set -eu

if [ $# -ne 3 ]; then
    sed -n '4p' "$0" >&2
    exit 2
fi
app=$1 out=$2 hash_list=$3
here=$(cd "$(dirname "$0")" && pwd -P)
python=${SFCOP_PYTHON:-python3}
[ -d "$app" ] || { echo "make_dmg: no app at $app" >&2; exit 1; }

staging="$out.staging"
rm -rf "$staging" "$out"
mkdir -p "$staging"
ditto "$app" "$staging/SFCOP.app"
ln -s /Applications "$staging/Applications"
hdiutil create -quiet -volname SFCOP -fs HFS+ -format UDZO -imagekey zlib-level=9 \
    -srcfolder "$staging" "$out"
rm -rf "$staging"
if ! "$python" -I "$here/../check_package.py" "$out" "$hash_list"; then
    rm -f "$out"
    exit 1
fi
echo "make_dmg: $out"
