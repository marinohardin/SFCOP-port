#!/bin/sh
# Build the Wine driver's programs with the P0-2 toolchain wrappers (VC6 SP4 under wibo, VC6 Lib/
# set from tools/vc6-lib). Output in work/oracle/build/out/:
#   oracle_input.exe        plays a steps file into the game
#   oracle_launch.exe       starts the game with the fixed-step clock (README.md)
#   oracle_clock.dll        that clock, loaded into the game by the launcher
#   oracle_clock_check.exe  reads the clock as the game does, for test_clock.sh
# ORACLE_DRIVER_BUILD names another folder for the objects and the output (its out/).
set -eu

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
root=${SFCOP_ROOT:-$(cd "$repo/.." && pwd)}
build=${ORACLE_DRIVER_BUILD:-$root/work/oracle/build}
work=$build/driver
out=$build/out
bin=$repo/toolchain/bin

mkdir -p "$work" "$out"
cp "$here/oracle_input.c" "$here/oracle_launch.c" "$here/oracle_clock.c" \
    "$here/oracle_clock_check.c" "$work/"
cd "$work"
# Z:/x is /x under wibo; a bare /Volumes/... path reads as an option.
for name in oracle_input oracle_launch oracle_clock oracle_clock_check; do
    "$bin/cl" /nologo /W3 /O2 /MT /c "/FoZ:$work/$name.obj" "Z:$work/$name.c"
done
"$bin/link" /nologo /subsystem:console /machine:I386 "/out:Z:$work/oracle_input.exe" \
    "Z:$work/oracle_input.obj" user32.lib gdi32.lib kernel32.lib
"$bin/link" /nologo /subsystem:console /machine:I386 "/out:Z:$work/oracle_launch.exe" \
    "Z:$work/oracle_launch.obj" kernel32.lib
# link.exe makes a DLL's export file by spawning "link -lib -def", which wibo cannot do; lib /def
# makes it first from the object's dllexport, and link.exe takes the .exp as an input.
"$bin/lib" /nologo /def /machine:I386 /name:oracle_clock.dll "/out:Z:$work/oracle_clock.lib" \
    "Z:$work/oracle_clock.obj"
"$bin/link" /nologo /dll /machine:I386 "/out:Z:$work/oracle_clock.dll" \
    "Z:$work/oracle_clock.exp" "Z:$work/oracle_clock.obj" user32.lib kernel32.lib
"$bin/link" /nologo /subsystem:console /machine:I386 "/out:Z:$work/oracle_clock_check.exe" \
    "Z:$work/oracle_clock_check.obj" user32.lib winmm.lib kernel32.lib
for file in oracle_input.exe oracle_launch.exe oracle_clock.dll oracle_clock_check.exe; do
    cp "$work/$file" "$out/"
    shasum -a 256 "$out/$file"
done
