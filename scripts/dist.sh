#!/bin/sh
# One command per platform for a runnable client and its release package (packaging/README.md).
#
#   sh scripts/dist.sh [PRESET] [BUILD_DIR]
#   sh scripts/dist.sh source
#
# PRESET is macos-arm64 (the default), linux-x64, linux-arm64, windows-x64 or windows-arm64. The
# preset is built with the lift in BUILD_DIR (default build/<PRESET>; an existing lift build
# directory is reused, and one configured without the lift is refused) and dist/ at the
# repository root, which git ignores, receives:
#
#   macos-arm64      dist/SFCOP.app                      dist/SFCOP-<version>-macos-arm64.dmg
#   linux-<arch>     dist/SFCOP-<version>-linux-<arch>/  dist/SFCOP-<version>-linux-<arch>.tar.gz
#                                                        dist/SFCOP-<version>-linux-<arch>.AppImage
#   windows-<arch>   dist/SFCOP-<version>-windows-<arch>/ dist/SFCOP-<version>-windows-<arch>.zip
#   source           dist/SFCOP-<version>-source.tar.gz (git archive of HEAD and of the pinned
#                    decomp/ commit; nothing is built)
#
# Every package is checked by packaging/check_package.py: nothing from the original game. The
# version is the VERSION file's, or SFCOP_VERSION. The install and the pcrecomp clone are found as
# cmake/SfcopLifter.cmake says, or given to the first configure by SFCOP_INSTALL_DIR and
# SFCOP_PCRECOMP_DIR. SFCOP_DIST_DIR moves dist/. The macOS client is linked for macOS 11 or later
# (SFCOP_MACOS_MIN). The packaged binaries carry no folder of the build machine: the build is a
# package build (SFCOP_PACKAGE_BUILD=ON), maps the
# remaining folders out (-ffile-prefix-map), the package strips debug information, and
# packaging/check_paths.py refuses the package otherwise (SFCOP_ALLOW_BUILD_PATHS=1 lets a local
# test package through, never in GitHub Actions). The AppImage needs Docker and is skipped with a
# warning when it cannot be made, unless SFCOP_APPIMAGE=required. Builds run at nice 5, -j4.
#
# The client is the lifted program (DESIGN §7.2): without the lift there is nothing to package,
# so SFCOP_LIFT=OFF is refused here; `source` is the package that needs no lift.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd -P)
case ${1:-} in
    macos-arm64 | linux-x64 | linux-arm64 | windows-x64 | windows-arm64 | source)
        preset=$1
        shift ;;
    *)
        # The older form, `dist.sh [BUILD_DIR]`, is macos-arm64.
        preset=macos-arm64 ;;
esac
version=${SFCOP_VERSION:-$(tr -d ' \n' <"$root/VERSION")}
dist=${SFCOP_DIST_DIR:-$root/dist}
python=${SFCOP_PYTHON:-python3}
export SFCOP_PYTHON="$python"
mkdir -p "$dist"

if [ "$preset" = source ]; then
    out="$dist/SFCOP-$version-source.tar.gz"
    top="SFCOP-$version-source"
    # git archive leaves the decomp submodule out, so the decompilation's pinned commit is
    # archived from its clone under decomp/ into the same package.
    pin=$(git -C "$root" rev-parse HEAD:decomp)
    if ! git -C "$root/decomp" cat-file -e "$pin^{commit}" 2>/dev/null; then
        echo "dist: the decompilation's pinned commit $pin is not in decomp/" \
             "(git submodule update --init)" >&2
        exit 2
    fi
    parts=$(mktemp -d "${TMPDIR:-/tmp}/sfcop-source.XXXXXX")
    trap 'rm -rf "$parts"' EXIT INT TERM
    git -C "$root" archive --format=tar --prefix="$top/" -o "$parts/port.tar" HEAD
    git -C "$root/decomp" archive --format=tar --prefix="$top/decomp/" -o "$parts/decomp.tar" "$pin"
    "$python" -I - "$out" "$parts/port.tar" "$parts/decomp.tar" <<'EOF'
import sys
import tarfile

out, *parts = sys.argv[1:]
seen = set()
with tarfile.open(out, 'w:gz', format=tarfile.PAX_FORMAT) as dst:
    for part in parts:
        with tarfile.open(part) as src:
            for member in src:
                if member.isdir() and member.name.rstrip('/') in seen:
                    continue    # the submodule's empty folder, written by both archives
                seen.add(member.name.rstrip('/'))
                dst.addfile(member, src.extractfile(member) if member.isfile() else None)
EOF
    echo "dist: $out"
    exit 0
fi

if [ "${SFCOP_LIFT:-ON}" != ON ]; then
    echo "dist: SFCOP_LIFT is ${SFCOP_LIFT}: the client is the lifted program, so there is nothing" \
         "to package without it; \`dist.sh source\` is the package without the lift" >&2
    exit 2
fi
case $preset in
    macos-arm64)
        if [ "$(uname -s)" != Darwin ]; then
            echo "dist: macos-arm64 is built on a Mac" >&2
            exit 2
        fi ;;
esac
build=${1:-"$root/build/$preset"}
case $preset in
    linux-* | windows-*)
        # A cross build's sysroot lives in the tools folder, which must be mapped out too.
        if [ -z "${SFCOP_TOOLS_DIR:-}" ] || [ ! -d "$SFCOP_TOOLS_DIR" ]; then
            echo "dist: set SFCOP_TOOLS_DIR to the tools folder setup.sh installed into: a $preset" \
                 "package build maps it out of the compiled code" >&2
            exit 2
        fi ;;
esac
# The oldest macOS the app runs on. Without it the client is linked for the SDK's own version, the
# Mac that builds it, and a package would not open on any older macOS.
macos_min=${SFCOP_MACOS_MIN:-11.0}
export SFCOP_VERSION="$version"

# The build machine's folders are mapped out of the compiled code (__FILE__, debug info): the
# checkout and the build folder to ".", the pcrecomp clone to "pcrecomp", the tools folder (the
# sysroots' headers) to "tools". The flags reach every
# target, SDL's included, through the compiler variables of the first configure.
build_abs=$(mkdir -p "$build" && cd "$build" && pwd -P)
maps="-ffile-prefix-map=$root=. -ffile-prefix-map=$build_abs=."
if [ -n "${SFCOP_PCRECOMP_DIR:-}" ] && [ -d "$SFCOP_PCRECOMP_DIR" ]; then
    maps="$maps -ffile-prefix-map=$(cd "$SFCOP_PCRECOMP_DIR" && pwd -P)=pcrecomp"
    if [ "$SFCOP_PCRECOMP_DIR" != "$(cd "$SFCOP_PCRECOMP_DIR" && pwd -P)" ]; then
        maps="$maps -ffile-prefix-map=$SFCOP_PCRECOMP_DIR=pcrecomp"
    fi
fi
if [ -n "${SFCOP_TOOLS_DIR:-}" ] && [ -d "$SFCOP_TOOLS_DIR" ]; then
    maps="$maps -ffile-prefix-map=$(cd "$SFCOP_TOOLS_DIR" && pwd -P)=tools"
    if [ "$SFCOP_TOOLS_DIR" != "$(cd "$SFCOP_TOOLS_DIR" && pwd -P)" ]; then
        maps="$maps -ffile-prefix-map=$SFCOP_TOOLS_DIR=tools"
    fi
fi
# The settings a package build needs: no build-machine folder in the configuration, and build
# runpaths relative to the binary ($ORIGIN) rather than the build folder. On Linux there are none
# at all, because sdl2-compat sets an absolute one of its own; the package's launcher and AppRun
# put its lib/ on the library path (packaging/linux/).
package_settings="-DSFCOP_PACKAGE_BUILD=ON -DCMAKE_BUILD_RPATH_USE_ORIGIN=ON"
skip_rpath=OFF
case $preset in
    linux-*) skip_rpath=ON ;;
esac
package_settings="$package_settings -DCMAKE_SKIP_BUILD_RPATH=$skip_rpath"

if [ ! -f "$build/CMakeCache.txt" ]; then
    export CFLAGS="${CFLAGS:+$CFLAGS }$maps" CXXFLAGS="${CXXFLAGS:+$CXXFLAGS }$maps"
    export OBJCFLAGS="${OBJCFLAGS:+$OBJCFLAGS }$maps" OBJCXXFLAGS="${OBJCXXFLAGS:+$OBJCXXFLAGS }$maps"
    # shellcheck disable=SC2086 # two -D words
    set -- -DSFCOP_LIFT=ON $package_settings
    if [ "$preset" = macos-arm64 ]; then
        set -- "$@" "-DCMAKE_OSX_DEPLOYMENT_TARGET=$macos_min"
    fi
    if [ -n "${SFCOP_INSTALL_DIR:-}" ]; then
        set -- "$@" "-DSFCOP_INSTALL_DIR=$SFCOP_INSTALL_DIR"
    fi
    if [ -n "${SFCOP_PCRECOMP_DIR:-}" ]; then
        set -- "$@" "-DSFCOP_PCRECOMP_DIR=$SFCOP_PCRECOMP_DIR"
    fi
    (cd "$root" && cmake --preset "$preset" -B "$build" "$@")
elif ! grep -q '^SFCOP_LIFT:BOOL=ON$' "$build/CMakeCache.txt"; then
    echo "dist: $build is configured without the lift (SFCOP_LIFT); give a lift build directory" >&2
    exit 2
elif ! grep -qF -e "$maps" "$build/CMakeCache.txt"; then
    echo "dist: $build was configured without these folder maps; remove its CMakeCache.txt (the" \
         "lift is kept), or give a new build directory" >&2
    exit 2
fi
if ! grep -q '^SFCOP_PACKAGE_BUILD:BOOL=ON$' "$build/CMakeCache.txt" ||
    ! grep -q '^CMAKE_BUILD_RPATH_USE_ORIGIN:BOOL=ON$' "$build/CMakeCache.txt" ||
    ! grep -q "^CMAKE_SKIP_BUILD_RPATH:BOOL=$skip_rpath\$" "$build/CMakeCache.txt"; then
    # An existing build folder is turned into a package build; the client recompiles and relinks.
    # shellcheck disable=SC2086
    cmake "$build" $package_settings
fi
if [ "$preset" = macos-arm64 ] &&
    ! grep -q "^CMAKE_OSX_DEPLOYMENT_TARGET:STRING=$macos_min\$" "$build/CMakeCache.txt"; then
    # A build directory made for another minimum (or none) is set to this one; it rebuilds.
    cmake "$build" "-DCMAKE_OSX_DEPLOYMENT_TARGET=$macos_min"
fi

# The decompilation holds the hash list (decomp/docs/INTERFACE.md).
hash_list="${SFCOP_DECOMP_DIR:-$root/decomp}/analysis/results/retail_hashes.sha256"
os=${preset%-*} arch=${preset#*-}
name="SFCOP-$version-$preset"

# find_built <file name>: the one file of that name the build wrote (sfcop's own folder first).
find_built() {
    if [ -f "$build/$1" ]; then
        echo "$build/$1"
        return 0
    fi
    _found=$(find "$build" -name "$1" \( -type f -o -type l \) -not -path '*/dist/*' | sort | head -n 1)
    [ -n "$_found" ] || { echo "dist: the build wrote no $1" >&2; exit 1; }
    echo "$_found"
}

# check_paths <binary>...: refuses a package whose binaries name a folder of this machine
# (packaging/check_paths.py prints counts only). SFCOP_ALLOW_BUILD_PATHS=1 lets a local test
# package through with a warning; the release workflow never sets it.
check_paths() {
    # check_paths.py decides SFCOP_ALLOW_BUILD_PATHS too, and refuses it in GitHub Actions.
    "$python" -I "$root/packaging/check_paths.py" "$build" "$@" || exit 1
}

case $os in
    macos)
        nice -n 5 cmake --build "$build" -j4 --target sfcop_app
        rm -rf "$dist/SFCOP.app"
        # ditto keeps the signature's extended attributes and the executable bits.
        ditto "$build/dist/SFCOP.app" "$dist/SFCOP.app"
        # The package carries no debug map: it names the build's object files. Stripping breaks
        # the ad hoc signatures, which are made again inside out, as make_bundle.sh makes them.
        app=$dist/SFCOP.app/Contents
        strip -S "$app/MacOS/sfcop" "$app/Frameworks/libSDL2-2.0.0.dylib" "$app/Frameworks/libSDL3.dylib"
        codesign --force --sign - --timestamp=none "$app/Frameworks/libSDL2-2.0.0.dylib" \
            "$app/Frameworks/libSDL3.dylib" "$app/MacOS/sfcop"
        codesign --force --sign - --timestamp=none "$dist/SFCOP.app"
        codesign --verify --strict --deep "$dist/SFCOP.app"
        check_paths "$app/MacOS/sfcop" "$app/Frameworks/libSDL2-2.0.0.dylib" "$app/Frameworks/libSDL3.dylib"
        echo "dist: $dist/SFCOP.app"
        min=$(vtool -show-build "$dist/SFCOP.app/Contents/MacOS/sfcop" | awk '$1 == "minos" { print $2; exit }')
        if [ "$min" != "$macos_min" ]; then
            echo "dist: the client is linked for macOS $min, not $macos_min" >&2
            exit 1
        fi
        sh "$root/packaging/macos/make_dmg.sh" "$dist/SFCOP.app" "$dist/$name.dmg" "$hash_list"
        ;;
    linux)
        nice -n 5 cmake --build "$build" -j4 --target sfcop
        sh "$root/packaging/linux/make_tree.sh" "$dist/$name" "$(find_built sfcop)" \
            "$(find_built libSDL2-2.0.so.0)" "$(find_built libSDL3.so.0)" "$root" "$version" "$arch"
        check_paths "$dist/$name/bin/sfcop" "$dist/$name/lib/libSDL2-2.0.so.0" "$dist/$name/lib/libSDL3.so.0"
        sh "$root/packaging/linux/make_tarball.sh" "$dist/$name" "$dist/$name.tar.gz" "$root"
        if ! sh "$root/packaging/linux/make_appimage.sh" "$dist/$name" "$dist/$name.AppImage" \
                "$arch" "$root"; then
            if [ "${SFCOP_APPIMAGE:-}" = required ]; then
                exit 1
            fi
            echo "dist: warning: no AppImage for $preset; the tar.gz is the Linux package" >&2
        fi
        ;;
    windows)
        nice -n 5 cmake --build "$build" -j4 --target sfcop
        sh "$root/packaging/windows/make_tree.sh" "$dist/$name" "$(find_built sfcop.exe)" \
            "$(find_built SDL2.dll)" "$(find_built SDL3.dll)" "$root" "$version" "$arch"
        check_paths "$dist/$name/sfcop.exe" "$dist/$name/SDL2.dll" "$dist/$name/SDL3.dll"
        sh "$root/packaging/windows/make_zip.sh" "$dist/$name" "$dist/$name.zip" "$root"
        ;;
esac
