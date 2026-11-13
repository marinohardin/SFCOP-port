#!/bin/sh
# The pcrecomp the build compiles against: pcrecomp at the pinned revision with the patches
# patches/series lists applied, in that order (the P0-4 static TLS hook, the three P1-2 win32hle
# patches, and the guest exception patches: landing pads for every catching function, the machine
# lock's depth for a longjmp out of nested guest calls, and a host hook for a zero divisor; and the
# machine's quantum, a count of hand-over points before an import gives the machine to a waiting
# thread; and for a Windows host, the runtime's POSIX calls on the Windows API, drive-letter host
# paths, the two kernel32 rows that read POSIX-only state, and no host CPUID on a Windows Arm host;
# and the dispatch lookup's cache, with a host's overrides written into the table, which indirect
# calls then ask before the manual hook; a host that writes nothing keeps the hook first; and
# landing pads that save no signal mask on a POSIX host, _setjmp and _longjmp).
#
#   cmake/lifter/setup.sh SOURCE
#       Make SOURCE a pcrecomp clone that holds the pinned revision: clone it (the only step that
#       uses the network) when SOURCE does not exist, else check it. SOURCE is never patched or
#       otherwise written; one clone serves every checkout and build folder on the machine.
#   cmake/lifter/setup.sh --copy SOURCE DEST STAMP
#       Make DEST, the build folder's own copy: a local clone of SOURCE (no network; SOURCE is only
#       read, its working tree and any change in it ignored) checked out at the pinned revision,
#       with the series applied. STAMP records the revision, this script and the series with every
#       patch's content as one hash; while it matches, nothing is done and nothing is touched, so
#       an unchanged build does not lift again. When it differs, or DEST no longer holds the
#       revision, DEST is made again from scratch and STAMP rewritten. DEST is a plain local clone
#       (no alternates), so moving SOURCE or a git gc in it never breaks a copy already made.
#       cmake/SfcopLifter.cmake runs this at every configure.
#
# Exit status: 0 done; 1 the clone or a patch failed; 2 patches/series does not match the
# patches in the folder (a configure error).
#
# SOURCE is regenerable and lives outside the repository, by default in
# $SFCOP_WORK/lifter-hle/pcrecomp; configure with -DSFCOP_PCRECOMP_DIR=SOURCE if it is elsewhere.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
PCRECOMP_URL=https://github.com/sp00nznet/pcrecomp
PCRECOMP_REV=ef3f0da6e08a8eb7a3f869273f3df071ca8d7373
SERIES=$HERE/patches/series

die() { echo "setup.sh: $*" >&2; exit 1; }
# A series that does not match the folder: exit status 2, which configure treats as an error.
die_series() { echo "setup.sh: $*" >&2; exit 2; }
sha256() { if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$@"; else sha256sum "$@"; fi; }

# The series' patch names, in order. Each must exist, and every patch in the folder must be listed.
series() {
    sed -e 's/#.*//' -e 's/[[:space:]]*$//' -e '/^$/d' "$SERIES"
}
check_series() {
    [ -f "$SERIES" ] || die_series "no $SERIES"
    for p in $(series); do
        [ -f "$HERE/patches/$p" ] || die_series "patches/series lists $p, which is not in cmake/lifter/patches"
        [ "$(series | grep -cFx "$p")" -eq 1 ] || die_series "patches/series lists $p more than once"
    done
    for f in "$HERE"/patches/*.patch; do
        [ -e "$f" ] || continue
        series | grep -qFx "$(basename "$f")" || die_series "$(basename "$f") is not listed in patches/series"
    done
}
# Revision, this script, then each listed patch's name and content: one hash.
stamp() {
    ( echo "pcrecomp $PCRECOMP_REV"; sha256 "$HERE/setup.sh" | cut -d' ' -f1
      for p in $(series); do echo "$p $(sha256 "$HERE/patches/$p" | cut -d' ' -f1)"; done ) \
        | sha256 | cut -d' ' -f1
}
has_rev() { git -C "$1" cat-file -e "$PCRECOMP_REV^{commit}" 2>/dev/null; }

check_series
if [ "${1:-}" = --copy ]; then
    [ $# -eq 4 ] || die "usage: setup.sh --copy SOURCE DEST STAMP"
    src=$2 dest=$3 stampfile=$4
    want=$(stamp)
    # Up to date: the stamp matches and the copy still holds its commit. The copy is a plain
    # local clone (objects hard-linked or copied, no alternates), so it does not depend on SOURCE
    # once made: moving SOURCE or running git gc in it leaves the copy whole.
    if [ -f "$stampfile" ] && [ "$(cat "$stampfile")" = "$want" ] \
        && git -C "$dest" cat-file -e "$PCRECOMP_REV^{commit}" 2>/dev/null; then
        exit 0
    fi
    [ -d "$src/.git" ] || die "no pcrecomp clone at $src: run cmake/lifter/setup.sh $src (or configure again with -DSFCOP_PCRECOMP_DIR=<where it is now>)"
    has_rev "$src" || die "the pcrecomp clone at $src does not hold $PCRECOMP_REV: fetch it there, then configure again"
    rm -f "$stampfile"
    rm -rf "$dest"
    git clone -q --no-checkout "$src" "$dest"
    git -C "$dest" -c advice.detachedHead=false checkout -q "$PCRECOMP_REV"
    for p in $(series); do
        git -C "$dest" apply "$HERE/patches/$p" || die "$p does not apply to pcrecomp $PCRECOMP_REV (fix it, then configure again)"
    done
    echo "$want" > "$stampfile"
    echo "setup.sh: pcrecomp $(echo "$PCRECOMP_REV" | cut -c1-12) with $(series | wc -l | tr -d ' ') patches in $dest"
    exit 0
fi

[ $# -eq 1 ] || die "usage: setup.sh SOURCE | setup.sh --copy SOURCE DEST STAMP"
src=$1
if [ ! -d "$src/.git" ]; then
    git clone -q --no-checkout "$PCRECOMP_URL" "$src"
    git -C "$src" -c advice.detachedHead=false checkout -q "$PCRECOMP_REV"
fi
has_rev "$src" || die "the pcrecomp clone at $src does not hold $PCRECOMP_REV: fetch it there"
echo "setup.sh: $src holds pcrecomp $(echo "$PCRECOMP_REV" | cut -c1-12); each build folder makes its own patched copy"
