#!/bin/sh
# One preset of the release workflow (.github/workflows/release.yml):
#
#   sh packaging/release_step.sh PRESET
#
# A cross preset first checks that its toolchain can be read (cmake/toolchains/check-readable.sh);
# a failure there fails this preset only. Then scripts/dist.sh PRESET runs in
# $RELEASE_ROOT/build/PRESET, and its whole output stays in $RELEASE_ROOT/logs/PRESET.log on the
# runner, not in the job's log: the configure and the lift can name the install's folder, which
# never goes to GitHub (the workflow also masks it). The job's log gets the packages made, or on
# failure the log's last 40 lines with the install's and the pcrecomp clone's folders, as given
# and as real paths, replaced by <install> and <pcrecomp>. Exits with the failing status.
set -u

[ $# -eq 1 ] || { sed -n '4p' "$0" >&2; exit 2; }
preset=$1
root=$(cd "$(dirname "$0")/.." && pwd -P)
log=$RELEASE_ROOT/logs/$preset.log
mkdir -p "$RELEASE_ROOT/logs"

if [ "$preset" != macos-arm64 ] && ! sh "$root/cmake/toolchains/check-readable.sh" "$preset"; then
    echo "$preset: not built: its toolchain cannot be read (above)"
    exit 1
fi

start=$(date +%s)
sh "$root/scripts/dist.sh" "$preset" "$RELEASE_ROOT/build/$preset" >"$log" 2>&1
status=$?
echo "$preset: dist.sh exit $status after $(( $(date +%s) - start )) s; the full log is $(basename "$log") in the release root's logs on the runner"
if [ "$status" -eq 0 ]; then
    grep -E '^(make_dmg|make_tarball|make_appimage|make_zip|dist): ' "$log" | sed 's|^\([a-z_]*\): .*/|\1: |' || true
else
    tail -n 40 "$log" | python3 -I "$root/packaging/scrub.py"
fi
exit "$status"
