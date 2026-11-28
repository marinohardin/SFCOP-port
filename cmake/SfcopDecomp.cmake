# The decompilation this build reads, only as decomp/docs/INTERFACE.md lists it: the op/ sources,
# op/manifest.tsv, the compile profiles (harness/units.json, harness/lib), toolchain/ (VC6),
# tools/manifest.py, ghidra/exports and analysis/results. Every
# reference from the build to it goes through SFCOP_DECOMP_DIR, by default the submodule decomp/.
#
# Provides:
#   SFCOP_DECOMP_DIR     the decompilation's folder (cache; -DSFCOP_DECOMP_DIR=<dir> for another)
#   SFCOP_DECOMP_SOURCE  its source root, ${SFCOP_DECOMP_DIR}/op
# Configure stops when the folder's INTERFACE_VERSION is missing or is not a version this build
# knows.
include_guard(GLOBAL)

set(SFCOP_DECOMP_DIR "${CMAKE_SOURCE_DIR}/decomp" CACHE PATH
    "The decompilation (the decomp submodule): its op/, harness/, toolchain/, ghidra/ and analysis/")
set(SFCOP_DECOMP_INTERFACE_KNOWN 5)

set(_sfcop_decomp_version_file "${SFCOP_DECOMP_DIR}/INTERFACE_VERSION")
if(NOT EXISTS "${_sfcop_decomp_version_file}")
    message(FATAL_ERROR "No decompilation at SFCOP_DECOMP_DIR (\"${SFCOP_DECOMP_DIR}\"): "
        "${_sfcop_decomp_version_file} is missing. Run `git submodule update --init`, or "
        "configure with -DSFCOP_DECOMP_DIR=<the decompilation's folder>.")
endif()
file(STRINGS "${_sfcop_decomp_version_file}" _sfcop_decomp_version LIMIT_COUNT 1)
string(STRIP "${_sfcop_decomp_version}" _sfcop_decomp_version)
if(NOT _sfcop_decomp_version STREQUAL "${SFCOP_DECOMP_INTERFACE_KNOWN}")
    message(FATAL_ERROR "The decompilation at SFCOP_DECOMP_DIR (\"${SFCOP_DECOMP_DIR}\") has "
        "interface version \"${_sfcop_decomp_version}\"; this build knows version "
        "${SFCOP_DECOMP_INTERFACE_KNOWN} (decomp/docs/INTERFACE.md). Check out the submodule "
        "commit this repository pins (`git submodule update --init`).")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_sfcop_decomp_version_file}")
set(SFCOP_DECOMP_SOURCE "${SFCOP_DECOMP_DIR}/op")
message(STATUS "sfcop: the decompilation is ${SFCOP_DECOMP_DIR} (interface version "
    "${_sfcop_decomp_version})")
