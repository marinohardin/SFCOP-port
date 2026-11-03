#!/bin/sh
# Build oracle/scr/OracleSkirmish into work/oracle/build/out/OracleSkirmish.scr with the P0-2/P1-8
# toolchain: toolchain/build_scr.sh, which uses the Release compile and link lines of
# OracleSkirmish.dsp (copied from the SDK's _ScriptTemplates/Multi template; one cl call per
# source) and works around the SDK's truncated dmp3D_MSVCR.lib and the export-file
# spawn limit (toolchain/README.md). MSVC_LIB_SET=initial (default) or sp6 picks the VC6 Lib/ set.
#
#   oracle/scr/build.sh     compile and link; prints the build summary and sha256
# Output: work/oracle/build/out/OracleSkirmish.scr, out/build.log
set -eu

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
root=${SFCOP_ROOT:-$(cd "$repo/.." && pwd)}
work=$root/work/oracle/build
out=$work/out
mkdir -p "$out"

image=${SFCOP_MSVC_IMAGE:-sfcop-msvc6:latest}
docker image inspect "$image" >/dev/null 2>&1 || {
    echo "build.sh: toolchain image $image not found; see toolchain/README.md (task P0-2)" >&2
    exit 3
}

"$repo/toolchain/build_scr.sh" "$here/OracleSkirmish" "$out/OracleSkirmish.scr" --jobs 4 \
    > "$out/build.log" 2>&1 || { tail -40 "$out/build.log"; echo "build.sh: FAILED, log in $out/build.log" >&2; exit 1; }
tail -5 "$out/build.log"
ls -l "$out/OracleSkirmish.scr"
shasum -a 256 "$out/OracleSkirmish.scr"
