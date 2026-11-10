#!/bin/sh
# Assembles SFCOP.app from a macOS build (cmake/SfcopBundle.cmake runs it as the target sfcop_app;
# scripts/dist.sh is the one command around it).
#
#   sh packaging/macos/make_bundle.sh OUT.app SFCOP SDL2_DYLIB SDL3_DYLIB SOURCE_DIR BUNDLE_ID VERSION
#
# OUT.app is rewritten every time, from a fresh copy beside it that replaces it only once it is
# complete, signed and checked. The bundle holds the client, its two SDL libraries in
# Contents/Frameworks with load paths inside the bundle, the hash list and the icon in
# Contents/Resources, and nothing from the original game: packaging/macos/check_bundle.py
# refuses the bundle otherwise. It is signed ad hoc, which lets its owner run it on this Mac; it
# is not yet a release package (scripts/dist.sh makes that). SFCOP_PYTHON names the Python for check_bundle.py (CMake passes
# its own); python3 on PATH otherwise.
set -eu

if [ $# -ne 7 ]; then
    sed -n '5p' "$0" >&2
    exit 2
fi
out=$1 sfcop=$2 sdl2=$3 sdl3=$4 source=$5 bundle_id=$6 project_version=$7
here=$(cd "$(dirname "$0")" && pwd -P)
hash_list="$source/analysis/results/retail_hashes.sha256"

for file in "$sfcop" "$sdl2" "$sdl3" "$hash_list" "$here/SFCOP.icns"; do
    if [ ! -f "$file" ]; then
        echo "make_bundle: missing $file" >&2
        exit 1
    fi
done

# The version: SFCOP_VERSION when scripts/dist.sh packages a release, else git describe of the
# source, the commit count as the build number (numeric, as CFBundleVersion wants); the project's
# version when the source is not a git checkout.
version=${SFCOP_VERSION:-$(git -C "$source" describe --tags --always --dirty 2>/dev/null || echo "$project_version")}
build_number=$(git -C "$source" rev-list --count HEAD 2>/dev/null || echo 0)
# The minimum macOS the client was linked for.
min_macos=$(vtool -show-build "$sfcop" 2>/dev/null | awk '$1 == "minos" { print $2; exit }')
min_macos=${min_macos:-11.0}

staging="$out.partial"
rm -rf "$staging"
contents="$staging/Contents"
mkdir -p "$contents/MacOS" "$contents/Frameworks" "$contents/Resources"

# Not named SFCOP: on a case-insensitive volume that is the client's own file.
cp "$here/launcher.sh" "$contents/MacOS/sfcop-launch"
chmod 755 "$contents/MacOS/sfcop-launch"
cp "$sfcop" "$contents/MacOS/sfcop"
# The build writes the SDL libraries with version links beside them; the bundle takes the files.
cp -L "$sdl2" "$contents/Frameworks/libSDL2-2.0.0.dylib"
cp -L "$sdl3" "$contents/Frameworks/libSDL3.dylib"
chmod 755 "$contents/MacOS/sfcop" "$contents/Frameworks/"*.dylib
cp "$hash_list" "$contents/Resources/retail_hashes.sha256"
cp "$here/SFCOP.icns" "$contents/Resources/SFCOP.icns"
sed -e "s|@BUNDLE_ID@|$bundle_id|" -e "s|@VERSION@|$version|" \
    -e "s|@BUILD_NUMBER@|$build_number|" -e "s|@MIN_MACOS@|$min_macos|" \
    "$here/Info.plist.in" >"$contents/Info.plist"
plutil -lint -s "$contents/Info.plist"
printf 'APPL????' >"$contents/PkgInfo"

# Load paths: every library a Mach-O in the bundle names outside the system is loaded from
# Contents/Frameworks through @rpath, and the build directory's rpaths are dropped. sdl2-compat
# itself loads libSDL3.dylib from its own folder (@loader_path), which is Frameworks too.
relink() {
    file=$1 rpath=$2
    otool -l "$file" | awk '$1 == "cmd" { cmd = $2 } cmd == "LC_RPATH" && $1 == "path" { print $2 }' |
        while IFS= read -r old; do
            install_name_tool -delete_rpath "$old" "$file" 2>/dev/null
        done
    if [ -n "$rpath" ]; then
        install_name_tool -add_rpath "$rpath" "$file" 2>/dev/null
    fi
    otool -L "$file" | tail -n +2 | awk '{ print $1 }' | while IFS= read -r dep; do
        case $dep in
        /usr/lib/* | /System/* | @executable_path/* | @loader_path/*) ;;
        @rpath/*) ;;
        *) install_name_tool -change "$dep" "@rpath/$(basename "$dep")" "$file" 2>/dev/null ;;
        esac
    done
}
# A library's own name, and the names the client links against, are its file name in Frameworks.
install_name_tool -id @rpath/libSDL2-2.0.0.dylib "$contents/Frameworks/libSDL2-2.0.0.dylib" 2>/dev/null
install_name_tool -id @rpath/libSDL3.dylib "$contents/Frameworks/libSDL3.dylib" 2>/dev/null
relink "$contents/MacOS/sfcop" @executable_path/../Frameworks
relink "$contents/Frameworks/libSDL2-2.0.0.dylib" @loader_path
relink "$contents/Frameworks/libSDL3.dylib" ""
for dep in $(otool -L "$contents/MacOS/sfcop" | awk '$1 ~ /^@rpath\// { print $1 }'); do
    name=${dep#@rpath/}
    if [ ! -f "$contents/Frameworks/$name" ]; then
        # The client links sdl2-compat by a version name; point it at the file the bundle has.
        case $name in
        libSDL2*) install_name_tool -change "$dep" @rpath/libSDL2-2.0.0.dylib "$contents/MacOS/sfcop" ;;
        libSDL3*) install_name_tool -change "$dep" @rpath/libSDL3.dylib "$contents/MacOS/sfcop" ;;
        *)
            echo "make_bundle: sfcop needs $dep, which the bundle does not carry" >&2
            exit 1
            ;;
        esac
    fi
done

# The relinking above hides install_name_tool's own messages (it warns that every change
# invalidates the signature) and runs inside pipelines, so a failed change would pass silently.
# This is the check that it worked: every dependency of the client and its libraries is the
# system's or inside the bundle, and the rpaths are exactly the intended ones.
assert_links() {
    file=$1 wanted=$2
    otool -L "$file" | tail -n +2 | awk '{ print $1 }' | while IFS= read -r dep; do
        case $dep in
        /usr/lib/* | /System/* | @rpath/* | @executable_path/* | @loader_path/*) ;;
        *)
            echo "make_bundle: $file depends on $dep, outside the system and the bundle" >&2
            exit 1
            ;;
        esac
    done
    found=$(otool -l "$file" | awk '$1 == "cmd" { cmd = $2 } cmd == "LC_RPATH" && $1 == "path" { print $2 }')
    if [ "$found" != "$wanted" ]; then
        echo "make_bundle: $file has rpaths [$found], wanted [$wanted]" >&2
        exit 1
    fi
}
assert_links "$contents/MacOS/sfcop" "@executable_path/../Frameworks"
assert_links "$contents/Frameworks/libSDL2-2.0.0.dylib" "@loader_path"
assert_links "$contents/Frameworks/libSDL3.dylib" ""

python="${SFCOP_PYTHON:-python3}"
"$python" "$here/check_bundle.py" "$staging" "$hash_list" >/dev/null

# Ad hoc signatures, inside out: the libraries, the client, then the bundle (which seals the
# launcher, the resources and Info.plist).
codesign --force --sign - --timestamp=none "$contents/Frameworks/libSDL2-2.0.0.dylib" \
    "$contents/Frameworks/libSDL3.dylib" "$contents/MacOS/sfcop"
codesign --force --sign - --timestamp=none "$staging"
codesign --verify --strict --deep "$staging"
"$python" "$here/check_bundle.py" "$staging" "$hash_list"

rm -rf "$out"
mv "$staging" "$out"
echo "make_bundle: $out ($version, macOS $min_macos or later)"
