#!/bin/sh
# Build oracle_input.exe with the P0-2 toolchain wrappers (VC6 SP4 under wibo, VC6 Lib/ set from
# tools/vc6-lib). Output: work/oracle/build/out/oracle_input.exe
set -eu

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
root=${SFCOP_ROOT:-$(cd "$repo/.." && pwd)}
work=$root/work/oracle/build/driver
out=$root/work/oracle/build/out
bin=$repo/toolchain/bin

mkdir -p "$work" "$out"
cp "$here/oracle_input.c" "$work/"
cd "$work"
# Z:/x is /x under wibo; a bare /Volumes/... path reads as an option.
"$bin/cl" /nologo /W3 /O2 /MT /c "/FoZ:$work/oracle_input.obj" "Z:$work/oracle_input.c"
"$bin/link" /nologo /subsystem:console /machine:I386 "/out:Z:$work/oracle_input.exe" \
    "Z:$work/oracle_input.obj" user32.lib gdi32.lib kernel32.lib
cp "$work/oracle_input.exe" "$out/"
shasum -a 256 "$out/oracle_input.exe"
