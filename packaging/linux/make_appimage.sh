#!/bin/sh
# Makes the Linux AppImage from the release tree make_tree.sh wrote (packaging/README.md):
#
#   sh packaging/linux/make_appimage.sh TREE OUT.AppImage ARCH SOURCE_DIR
#
# ARCH is x64 or arm64. The AppDir (AppRun, the desktop entry and icon, usr/bin/sfcop with the hash
# list beside it, usr/lib/ with the SDL libraries) is checked by packaging/check_package.py, packed
# by the pinned appimagetool with the pinned type 2 runtime of ARCH in the linux-arm64 run image
# (no network), then unpacked again with the AppImage's own --appimage-extract in a container of
# ARCH and checked once more. Needs Docker and the tools fetch_appimage_tools.sh downloads.
set -eu

if [ $# -ne 4 ]; then
    sed -n '4p' "$0" >&2
    exit 2
fi
tree=$1 out=$2 arch=$3 source=$4
# The decompilation holds the hash list and the install manifest (decomp/docs/INTERFACE.md).
decomp=${SFCOP_DECOMP_DIR:-$source/decomp}
here=$(cd "$(dirname "$0")" && pwd -P)
python=${SFCOP_PYTHON:-python3}
hash_list="$decomp/analysis/results/retail_hashes.sha256"
. "$here/appimage.env"
. "$source/cmake/toolchains/pins.env"
case $arch in
    x64) runtime_arch=x86_64 platform=linux/amd64 ;;
    arm64) runtime_arch=aarch64 platform=linux/arm64 ;;
    *) echo "make_appimage: ARCH is x64 or arm64, not $arch" >&2; exit 2 ;;
esac
tools=$(sh "$here/fetch_appimage_tools.sh")
command -v docker >/dev/null 2>&1 || { echo "make_appimage: Docker is needed" >&2; exit 1; }

work="$out.work"
rm -rf "$work" "$out"
appdir="$work/SFCOP.AppDir"
mkdir -p "$appdir/usr/bin" "$appdir/usr/lib" "$appdir/usr/share/doc/sfcop"
cp "$here/AppRun" "$appdir/AppRun"
cp "$tree/bin/sfcop" "$tree/bin/retail_hashes.sha256" "$tree/bin/install_manifest.tsv" \
    "$appdir/usr/bin/"
cp "$tree/lib/libSDL2-2.0.so.0" "$tree/lib/libSDL3.so.0" "$appdir/usr/lib/"
cp "$tree/LICENSE.txt" "$tree/LICENSE-EXCEPTION.txt" "$appdir/usr/share/doc/sfcop/"
cp "$tree/share/applications/sfcop.desktop" "$appdir/sfcop.desktop"
cp "$tree/share/icons/hicolor/512x512/apps/sfcop.png" "$appdir/sfcop.png"
cp "$appdir/sfcop.png" "$appdir/.DirIcon"
chmod 755 "$appdir/AppRun"
"$python" -I "$here/../check_package.py" --kind appdir "$appdir" "$hash_list"

case "$work$tools" in
    *" "*) echo "make_appimage: paths with spaces cannot be mounted: $work, $tools" >&2; exit 2 ;;
esac
name=$(basename "$out")
file_root=/tools/file-$FILE_DEB_VERSION
nice -n 19 docker run --rm --platform linux/arm64 --network none --user "$(id -u):$(id -g)" \
    -e HOME=/tmp -e APPIMAGE_EXTRACT_AND_RUN=1 -e ARCH="$runtime_arch" \
    -e PATH="$file_root/usr/bin:/usr/local/bin:/usr/bin:/bin" \
    -e LD_LIBRARY_PATH="$file_root/usr/lib/aarch64-linux-gnu" \
    -e MAGIC="$file_root/usr/lib/file/magic.mgc" \
    -v "$tools:/tools:ro" -v "$work:/work" -w /work "$LINUX_ARM64_RUN_IMAGE" \
    "/tools/appimagetool-$APPIMAGETOOL_VERSION-aarch64.AppImage" --no-appstream \
    --runtime-file "/tools/runtime-$APPIMAGE_RUNTIME_VERSION-$runtime_arch" \
    /work/SFCOP.AppDir "/work/$name"

# What the AppImage unpacks to is what a player runs: check it, not only the AppDir.
image=$LINUX_RUN_IMAGE
[ "$arch" = arm64 ] && image=$LINUX_ARM64_RUN_IMAGE
mkdir -p "$work/extract"
# Docker's emulation of another architecture refuses to start an ELF file with the AppImage
# marker in its header padding (bytes 8-10, "AI\2"); a Linux kernel ignores it. A copy with the
# marker cleared unpacks the same image.
cp "$work/$name" "$work/unpacker"
printf '\000\000\000' | dd of="$work/unpacker" bs=1 seek=8 conv=notrunc 2>/dev/null
nice -n 19 docker run --rm --platform "$platform" --network none --user "$(id -u):$(id -g)" \
    -e HOME=/tmp -v "$work:/work" -w /work/extract "$image" \
    /work/unpacker --appimage-extract >/dev/null
"$python" -I "$here/../check_package.py" --kind appdir "$work/extract/squashfs-root" "$hash_list"

mv "$work/$name" "$out"
rm -rf "$work"
echo "make_appimage: $out"
