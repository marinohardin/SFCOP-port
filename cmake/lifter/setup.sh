#!/bin/sh
# Make the pcrecomp clone the build needs: pcrecomp at the pinned revision with this directory's
# patches applied (the P0-4 static TLS hook, the three P1-2 win32hle patches, and the guest
# exception patches: landing pads for every catching function, the machine lock's depth for a
# longjmp out of nested guest calls, and a host hook for a zero divisor; and the machine's
# quantum, a count of hand-over points before an import gives the machine to a waiting thread).
# The clone is regenerable and lives outside the repository, by default in
# $SFCOP_WORK/lifter-hle/pcrecomp.
#
#   cmake/lifter/setup.sh DIR       clone into DIR (or check and patch an existing clone)
#
# Then configure with -DSFCOP_PCRECOMP_DIR=DIR if DIR is not the default.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
PCRECOMP_URL=https://github.com/sp00nznet/pcrecomp
PCRECOMP_REV=ef3f0da6e08a8eb7a3f869273f3df071ca8d7373
[ $# -eq 1 ] || { echo "usage: setup.sh DIR" >&2; exit 2; }
dir=$1
[ -d "$dir/.git" ] || git clone -q "$PCRECOMP_URL" "$dir"
if [ "$(git -C "$dir" rev-parse HEAD)" != "$PCRECOMP_REV" ]; then
    git -C "$dir" checkout -q "$PCRECOMP_REV"
fi
# The order matters: the manual lookup hook applies on top of the TIB hook.
for p in "$HERE/patches/pcrecomp-tib-hook.patch" \
         "$HERE/patches/pcrecomp-kernel32-unnamed-objects.patch" \
         "$HERE/patches/pcrecomp-manual-lookup-hook.patch" \
         "$HERE/patches/pcrecomp-wsock32-select-accept.patch" \
         "$HERE/patches/pcrecomp-eh-landing-pads.patch" \
         "$HERE/patches/pcrecomp-mach-depth.patch" \
         "$HERE/patches/pcrecomp-div-fault.patch" \
         "$HERE/patches/pcrecomp-quantum.patch"; do
    if git -C "$dir" apply --check "$p" 2>/dev/null; then
        git -C "$dir" apply "$p"; echo "applied $(basename "$p")"
    elif git -C "$dir" apply --reverse --check "$p" 2>/dev/null; then
        echo "already applied $(basename "$p")"
    else
        echo "does not apply: $p" >&2; exit 1
    fi
done
