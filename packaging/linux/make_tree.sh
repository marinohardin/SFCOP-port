#!/bin/sh
# Assembles the Linux release tree from a linux-x64 or linux-arm64 build (packaging/README.md):
#
#   sh packaging/linux/make_tree.sh OUT_DIR SFCOP SDL2_SO SDL3_SO SOURCE_DIR VERSION ARCH
#
# OUT_DIR is rewritten: sfcop (the launcher), bin/sfcop and the hash list beside it,
# lib/libSDL2-2.0.so.0 and lib/libSDL3.so.0, the desktop entry and icon under share/, README.txt, LICENSE.txt, LICENSE-EXCEPTION.txt.
# Nothing from the original game: packaging/check_package.py refuses the tree otherwise. ARCH is
# x64 or arm64. With SFCOP_LLVM_DIR (or the pinned LLVM under SFCOP_TOOLS_DIR) the client's
# needed libraries are checked: only glibc's and the two SDL libraries.
set -eu

if [ $# -ne 7 ]; then
    sed -n '4p' "$0" >&2
    exit 2
fi
out=$1 sfcop=$2 sdl2=$3 sdl3=$4 source=$5 version=$6 arch=$7
# The decompilation holds the hash list and the install manifest (decomp/docs/INTERFACE.md).
decomp=${SFCOP_DECOMP_DIR:-$source/decomp}
here=$(cd "$(dirname "$0")" && pwd -P)
python=${SFCOP_PYTHON:-python3}
hash_list="$decomp/analysis/results/retail_hashes.sha256"
manifest="$decomp/analysis/results/install_manifest.tsv"
case $arch in
    x64) glibc=2.31 ;;
    arm64) glibc=2.36 ;;
    *) echo "make_tree: ARCH is x64 or arm64, not $arch" >&2; exit 2 ;;
esac
for file in "$sfcop" "$sdl2" "$sdl3" "$hash_list" "$manifest" "$source/LICENSE" \
    "$source/LICENSE-EXCEPTION"; do
    [ -f "$file" ] || { echo "make_tree: missing $file" >&2; exit 1; }
done

staging="$out.partial"
rm -rf "$staging"
mkdir -p "$staging/bin" "$staging/lib" "$staging/share/applications" \
    "$staging/share/icons/hicolor/512x512/apps"
cp "$here/sfcop.sh" "$staging/sfcop"
cp "$sfcop" "$staging/bin/sfcop"
cp "$hash_list" "$staging/bin/retail_hashes.sha256"
# The import's manifest, beside the hash list, where the client looks for it.
cp "$manifest" "$staging/bin/install_manifest.tsv"
# The build writes the libraries with version links; the tree takes the files under their sonames.
cp -L "$sdl2" "$staging/lib/libSDL2-2.0.so.0"
cp -L "$sdl3" "$staging/lib/libSDL3.so.0"
cp "$here/sfcop.desktop" "$staging/share/applications/sfcop.desktop"
"$python" -I "$here/write_icon.py" "$staging/share/icons/hicolor/512x512/apps/sfcop.png"
sed -e "s|@VERSION@|$version|" -e "s|@ARCH@|$arch|" -e "s|@GLIBC@|$glibc|" \
    "$here/README.txt.in" >"$staging/README.txt"
# The licence and its section 7 permission travel with every binary (README.md, "Status and legal").
cp "$source/LICENSE" "$staging/LICENSE.txt"
cp "$source/LICENSE-EXCEPTION" "$staging/LICENSE-EXCEPTION.txt"
chmod 755 "$staging/sfcop" "$staging/bin/sfcop" "$staging/lib/"*.so.0
chmod 644 "$staging/bin/retail_hashes.sha256" "$staging/bin/install_manifest.tsv" "$staging/README.txt" \
    "$staging/LICENSE.txt" "$staging/LICENSE-EXCEPTION.txt" \
    "$staging/share/applications/sfcop.desktop" "$staging/share/icons/hicolor/512x512/apps/sfcop.png"

llvm=${SFCOP_LLVM_DIR:-}
if [ -z "$llvm" ]; then
    if [ -z "${SFCOP_TOOLS_DIR:-}" ]; then
        echo "make_tree: set SFCOP_TOOLS_DIR (the tools folder setup.sh installed into), or" \
             "SFCOP_LLVM_DIR" >&2
        exit 2
    fi
    . "$source/cmake/toolchains/pins.env"
    llvm=$SFCOP_TOOLS_DIR/llvm/$LLVM_VERSION
fi
# No debug information in the package: it names the build machine's folders.
if [ -x "$llvm/bin/llvm-strip" ]; then
    "$llvm/bin/llvm-strip" --strip-debug "$staging/bin/sfcop" "$staging/lib/libSDL2-2.0.so.0" \
        "$staging/lib/libSDL3.so.0"
else
    echo "make_tree: note: no llvm-strip; the debug information was left in" >&2
fi

# The client's needed libraries: glibc's own and the two the tree carries, nothing else.
if [ -x "$llvm/bin/llvm-readobj" ]; then
    "$llvm/bin/llvm-readobj" --needed-libs "$staging/bin/sfcop" "$staging/lib/libSDL2-2.0.so.0" |
        awk '/^ +lib/ { print $1 }' | sort -u | while IFS= read -r lib; do
        case $lib in
            libc.so.6 | libm.so.6 | libdl.so.2 | libpthread.so.0 | librt.so.1 | ld-linux*) ;;
            libSDL2-2.0.so.0 | libSDL3.so.0) ;;
            *) echo "make_tree: the client needs $lib, which neither glibc nor the tree gives" >&2
               exit 1 ;;
        esac
    done
else
    echo "make_tree: note: no llvm-readobj; the needed libraries were not checked" >&2
fi

"$python" -I "$here/../check_package.py" --kind linux "$staging" "$hash_list"
rm -rf "$out"
mv "$staging" "$out"
echo "make_tree: $out"
