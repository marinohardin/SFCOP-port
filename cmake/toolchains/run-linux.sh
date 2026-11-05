#!/bin/sh
# linux-x64's CMAKE_CROSSCOMPILING_EMULATOR: runs a Linux x86_64 program from the Mac in the
# pinned container (LINUX_RUN_IMAGE in pins.env: Debian 11, glibc 2.31, as the sysroot).
#
#   sh cmake/toolchains/run-linux.sh <program> [arguments...]
#
# The program's folder, the current folder and the checkout (read-only) are mounted at the same
# paths, so the absolute paths ctest and the tests use still work. SFCOP_RUN_MOUNTS adds folders (colon-separated, mounted
# read-only at the same paths). The container has no network and runs as the calling user.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=pins.env
. "$here/pins.env"
[ $# -ge 1 ] || { echo "usage: run-linux.sh <program> [arguments...]" >&2; exit 2; }
prog_dir=$(cd "$(dirname "$1")" && pwd)
prog=$prog_dir/$(basename "$1")
shift
cwd=$(pwd)

source_dir=$(cd "$here/../.." && pwd)

set -- "$prog" "$@"
mounts="-v $prog_dir:$prog_dir"
case "$cwd/" in
    "$prog_dir"/*) ;;
    *) mounts="$mounts -v $cwd:$cwd" ;;
esac
# The checkout, read-only: the tests read their fixtures from it by absolute path.
case "$prog_dir/" in
    "$source_dir"/*) ;;
    *) mounts="$mounts -v $source_dir:$source_dir:ro" ;;
esac
old_ifs=$IFS
IFS=:
for extra in ${SFCOP_RUN_MOUNTS:-}; do
    [ -n "$extra" ] && mounts="$mounts -v $extra:$extra:ro"
done
IFS=$old_ifs

# shellcheck disable=SC2086 # $mounts is a list of options; the paths have no spaces (checked).
case "$prog_dir$cwd$source_dir${SFCOP_RUN_MOUNTS:-}" in
    *" "*) echo "run-linux.sh: paths with spaces cannot be mounted: $prog_dir, $cwd" >&2; exit 2 ;;
esac
exec docker run --rm --platform linux/amd64 --network none \
    --user "$(id -u):$(id -g)" -e HOME=/tmp -e SFCOP_SKIP_TIMING_TESTS \
    $mounts -w "$cwd" "$LINUX_RUN_IMAGE" "$@"
