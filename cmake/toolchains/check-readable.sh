#!/bin/sh
# Fails within seconds, with the cause, when the cross toolchains cannot be read from where the
# job runs. CI runs it before the cross presets configure (README.md, "The runner").
#
#   sh cmake/toolchains/check-readable.sh <windows-x64|linux-x64>
#
# On macOS the first read of an external volume by a process the runner started makes the system
# ask for permission ("Removable Volumes", for the runner's own node). The question appears on the
# Mac's screen and blocks the read until someone answers, so on a Mac nobody is watching the job
# hangs inside the first compiler run: PR 50's first two runs lost 45 minutes each to it. A read
# that has not finished in 20 seconds is reported as that.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=pins.env
. "$here/pins.env"
[ $# -eq 1 ] || { echo "usage: check-readable.sh <windows-x64|linux-x64>" >&2; exit 2; }
tools=${SFCOP_TOOLS_DIR:-$SFCOP_ROOT/tools}
llvm=${SFCOP_LLVM_DIR:-$tools/llvm/$LLVM_VERSION}
case $1 in
    linux-x64)
        sysroot=${SFCOP_LINUX_SYSROOT:-$tools/sysroots/linux-x64/steamrt3-sniper-$LINUX_SYSROOT_VERSION}
        probe=$sysroot/usr/include/stdio.h ;;
    windows-x64)
        sysroot=${SFCOP_WINDOWS_SYSROOT:-$tools/sysroots/windows-x64/msvc-$WINDOWS_CRT_VERSION-sdk-$WINDOWS_SDK_VERSION}
        probe=$sysroot/crt/include/vcruntime.h ;;
    *) echo "check-readable.sh: no cross preset $1" >&2; exit 2 ;;
esac

# The runner's node is the process macOS holds responsible: <runner>/externals/node*/bin/node.
runner=$(cd "${RUNNER_TEMP:-.}/../.." 2>/dev/null && pwd || echo "the runner's folder")

for file in "$llvm/bin/clang-${LLVM_VERSION%%.*}" "$probe"; do
    if [ ! -e "$file" ]; then
        echo "check-readable: $file does not exist. Install the toolchains with" \
             "'sh cmake/toolchains/setup.sh', or set SFCOP_TOOLS_DIR (README.md, \"The runner\")." >&2
        exit 1
    fi
    if ! perl -e 'alarm 20; exec @ARGV' head -c 1 "$file" > /dev/null 2>&1; then
        echo "check-readable: reading $file did not finish in 20 seconds (or failed)." >&2
        echo "On macOS this is the 'Removable Volumes' permission: the system is waiting for an" >&2
        echo "answer on the Mac's screen, or it was refused. Open System Settings > Privacy &" >&2
        echo "Security > Files & Folders (or Full Disk Access) and allow node under" >&2
        echo "$runner/externals to read removable volumes. A runner update that moves node to a new" >&2
        echo "folder needs the permission again." >&2
        exit 1
    fi
done
echo "check-readable: the $1 toolchain is readable from here"
