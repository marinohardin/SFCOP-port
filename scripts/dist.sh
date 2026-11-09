#!/bin/sh
# One command for the owner's own SFCOP.app (DESIGN §12.2: local packaging, not a release).
#
#   sh scripts/dist.sh [BUILD_DIR]
#
# Builds the macos-arm64 preset with the lift in BUILD_DIR (default build/macos-arm64, the
# preset's own directory; pass an existing configured build directory to reuse it) and writes
# dist/SFCOP.app at the repository root, which git ignores. A build directory that has not been
# configured is configured with the preset first; the install and the pcrecomp clone are found as
# cmake/SfcopLifter.cmake says, or given to the first configure by SFCOP_INSTALL_DIR and
# SFCOP_PCRECOMP_DIR in the environment. A second run with nothing changed rebuilds nothing and
# rewrites the bundle.
#
# Then double-click dist/SFCOP.app, or: open dist/SFCOP.app --args --windowed
# Windows and Linux packaging will be cases of this script; it builds macOS only for now.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd -P)
build=${1:-"$root/build/macos-arm64"}

if [ "$(uname -s)" != Darwin ]; then
    echo "dist: only macOS is packaged so far" >&2
    exit 2
fi

if [ ! -f "$build/CMakeCache.txt" ]; then
    set -- -DSFCOP_LIFT=ON
    if [ -n "${SFCOP_INSTALL_DIR:-}" ]; then
        set -- "$@" "-DSFCOP_INSTALL_DIR=$SFCOP_INSTALL_DIR"
    fi
    if [ -n "${SFCOP_PCRECOMP_DIR:-}" ]; then
        set -- "$@" "-DSFCOP_PCRECOMP_DIR=$SFCOP_PCRECOMP_DIR"
    fi
    (cd "$root" && cmake --preset macos-arm64 -B "$build" "$@")
elif ! grep -q '^SFCOP_LIFT:BOOL=ON$' "$build/CMakeCache.txt"; then
    echo "dist: $build is configured without the lift (SFCOP_LIFT); give a lift build directory" >&2
    exit 2
fi

nice -n 19 cmake --build "$build" -j4 --target sfcop_app

mkdir -p "$root/dist"
rm -rf "$root/dist/SFCOP.app"
# ditto keeps the signature's extended attributes and the executable bits.
ditto "$build/dist/SFCOP.app" "$root/dist/SFCOP.app"
codesign --verify --strict --deep "$root/dist/SFCOP.app"
echo "dist: $root/dist/SFCOP.app"
