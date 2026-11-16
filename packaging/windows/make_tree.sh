#!/bin/sh
# Assembles the Windows release folder from a windows-x64 or windows-arm64 build
# (packaging/README.md):
#
#   sh packaging/windows/make_tree.sh OUT_DIR SFCOP_EXE SDL2_DLL SDL3_DLL SOURCE_DIR VERSION ARCH
#
# OUT_DIR is rewritten: sfcop.exe with SDL2.dll and SDL3.dll beside it (Windows loads a program's
# DLLs from its own folder first), the hash list beside sfcop.exe, README.txt (CRLF line ends).
# The CRT is linked statically, so no Visual C++ redistributable is needed. Nothing from the
# original game: packaging/check_package.py refuses the folder otherwise. ARCH is x64 or arm64.
set -eu

if [ $# -ne 7 ]; then
    sed -n '5p' "$0" >&2
    exit 2
fi
out=$1 exe=$2 sdl2=$3 sdl3=$4 source=$5 version=$6 arch=$7
# The decompilation holds the hash list and the install manifest (decomp/docs/INTERFACE.md).
decomp=${SFCOP_DECOMP_DIR:-$source/decomp}
here=$(cd "$(dirname "$0")" && pwd -P)
python=${SFCOP_PYTHON:-python3}
hash_list="$decomp/analysis/results/retail_hashes.sha256"
manifest="$decomp/analysis/results/install_manifest.tsv"
case $arch in
    x64 | arm64) ;;
    *) echo "make_tree: ARCH is x64 or arm64, not $arch" >&2; exit 2 ;;
esac
for file in "$exe" "$sdl2" "$sdl3" "$hash_list" "$manifest"; do
    [ -f "$file" ] || { echo "make_tree: missing $file" >&2; exit 1; }
done

staging="$out.partial"
rm -rf "$staging"
mkdir -p "$staging"
cp "$exe" "$staging/sfcop.exe"
cp "$sdl2" "$staging/SDL2.dll"
cp "$sdl3" "$staging/SDL3.dll"
cp "$hash_list" "$staging/retail_hashes.sha256"
# The import's manifest, beside the hash list, where the client looks for it.
cp "$manifest" "$staging/install_manifest.tsv"
sed -e "s|@VERSION@|$version|" -e "s|@ARCH@|$arch|" -e 's/$/\r/' "$here/README.txt.in" \
    >"$staging/README.txt"
chmod 755 "$staging/sfcop.exe" "$staging/SDL2.dll" "$staging/SDL3.dll"
chmod 644 "$staging/retail_hashes.sha256" "$staging/install_manifest.tsv" "$staging/README.txt"
"$python" -I "$here/../check_package.py" --kind windows "$staging" "$hash_list"
rm -rf "$out"
mv "$staging" "$out"
echo "make_tree: $out"
