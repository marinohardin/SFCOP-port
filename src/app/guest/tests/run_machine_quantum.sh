#!/bin/sh
# Builds MachineQuantumTest.cpp against pcrecomp's host_lite.c and runs it (../README.md,
# "Tests"). It needs no lifted program, only a pcrecomp clone (cmake/lifter/setup.sh) and doctest's
# headers (a configured build tree has them under _deps/doctest-src). The clone is only read: the
# test compiles against SCRATCH/pcrecomp, a copy with cmake/lifter/patches/series applied
# (setup.sh --copy), which the quantum patch is part of.
#
#   run_machine_quantum.sh PCRECOMP DOCTEST SCRATCH
#
# Passes when every case passes: doctest's own summary, and its exit status.
set -eu
pcrecomp=${1:?usage: run_machine_quantum.sh PCRECOMP DOCTEST SCRATCH}
doctest=${2:?}
scratch=${3:?}
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$scratch"
sh "$here/../../../../cmake/lifter/setup.sh" --copy "$pcrecomp" "$scratch/pcrecomp" "$scratch/pcrecomp.stamp"
pcrecomp=$scratch/pcrecomp

# As the build compiles the runtime (cmake/SfcopLifter.cmake, SFCOP_RECOMP_DEFINITIONS).
defs="-DRECOMP_RETADDR=0u -DRECOMP_LOCAL_REGS -DRECOMP_MEM_BASE=0x100000000000ull"
inc="-I$pcrecomp/runtime/win32hle -I$pcrecomp/runtime/recomp32"
# shellcheck disable=SC2086 # $defs and $inc are lists of flags
cc -O1 -g $defs $inc -fno-strict-aliasing -fwrapv -w \
    -c "$pcrecomp/runtime/win32hle/host_lite.c" -o "$scratch/host_lite.o"
# shellcheck disable=SC2086
c++ -std=c++17 -O1 -g $defs $inc -isystem "$doctest" \
    -c "$here/MachineQuantumTest.cpp" -o "$scratch/MachineQuantumTest.o"
c++ "$scratch/MachineQuantumTest.o" "$scratch/host_lite.o" -o "$scratch/machine_quantum" -lpthread
"$scratch/machine_quantum"
