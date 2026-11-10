#!/bin/sh
# Downloads the pinned AppImage tools (appimage.env) into SFCOP_APPIMAGE_TOOLS (default
# $SFCOP_TOOLS_DIR/appimage), once; a file whose SHA-256 is not the pin is refused and removed.
#
#   sh packaging/linux/fetch_appimage_tools.sh
#
# Prints the folder. The release workflow runs it before it packages Linux.
set -eu

here=$(cd "$(dirname "$0")" && pwd -P)
. "$here/appimage.env"
if [ -n "${SFCOP_APPIMAGE_TOOLS:-}" ]; then
    dest=$SFCOP_APPIMAGE_TOOLS
elif [ -n "${SFCOP_TOOLS_DIR:-}" ]; then
    dest=$SFCOP_TOOLS_DIR/appimage
else
    echo "fetch_appimage_tools: set SFCOP_APPIMAGE_TOOLS, or SFCOP_TOOLS_DIR (the tools folder)" >&2
    exit 2
fi
mkdir -p "$dest"
sha256() { shasum -a 256 "$1" | cut -c1-64; }

fetch() {
    _url=$1 _file=$dest/$2 _sha=$3
    if [ -f "$_file" ] && [ "$(sha256 "$_file")" = "$_sha" ]; then
        return 0
    fi
    echo "fetch_appimage_tools: downloading $2 from $_url" >&2
    rm -f "$_file.part"
    curl -fsSL --retry 3 --proto '=https' -o "$_file.part" "$_url"
    _got=$(sha256 "$_file.part")
    if [ "$_got" != "$_sha" ]; then
        rm -f "$_file.part"
        echo "fetch_appimage_tools: $2: SHA-256 $_got, pinned $_sha; refused and removed" >&2
        exit 1
    fi
    chmod 755 "$_file.part"
    mv "$_file.part" "$_file"
}

fetch "$APPIMAGETOOL_URL" "appimagetool-$APPIMAGETOOL_VERSION-aarch64.AppImage" "$APPIMAGETOOL_SHA256"
fetch "$APPIMAGE_RUNTIME_X86_64_URL" "runtime-$APPIMAGE_RUNTIME_VERSION-x86_64" "$APPIMAGE_RUNTIME_X86_64_SHA256"
fetch "$APPIMAGE_RUNTIME_AARCH64_URL" "runtime-$APPIMAGE_RUNTIME_VERSION-aarch64" "$APPIMAGE_RUNTIME_AARCH64_SHA256"

# file and libmagic for appimagetool (appimage.env): the three pinned packages, unpacked once into
# file-<version>/ and mounted beside appimagetool.
fetch "$FILE_DEB_URL" "file_$FILE_DEB_VERSION.deb" "$FILE_DEB_SHA256"
fetch "$LIBMAGIC1_DEB_URL" "libmagic1_$FILE_DEB_VERSION.deb" "$LIBMAGIC1_DEB_SHA256"
fetch "$LIBMAGIC_MGC_DEB_URL" "libmagic-mgc_$FILE_DEB_VERSION.deb" "$LIBMAGIC_MGC_DEB_SHA256"
file_root=$dest/file-$FILE_DEB_VERSION
file_ok() {
    [ -x "$file_root/usr/bin/file" ] && [ -f "$file_root/usr/lib/file/magic.mgc" ] &&
        [ -e "$file_root/usr/lib/aarch64-linux-gnu/libmagic.so.1" ]
}
if ! file_ok; then
    rm -rf "$file_root" "$file_root.part"
    mkdir -p "$file_root.part"
    for deb in file libmagic1 libmagic-mgc; do
        # A .deb is an ar archive holding data.tar.xz. This relies on bsdtar, the tar of macOS and
        # so of the release runner, which reads ar archives; GNU tar does not.
        tar -xOf "$dest/${deb}_$FILE_DEB_VERSION.deb" data.tar.xz | tar -xJf - -C "$file_root.part"
    done
    mv "$file_root.part" "$file_root"
    file_ok || { echo "fetch_appimage_tools: the file packages lack file, magic.mgc or libmagic" >&2; exit 1; }
fi
echo "$dest"
