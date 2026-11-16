# The object lift (DESIGN §6.3, §7.2): the functions the decompilation's op/manifest.tsv
# (${SFCOP_DECOMP_DIR}, cmake/SfcopDecomp.cmake) claims for VC6 enter the native build as their
# own VC6 objects, lifted, in place of the exe lift's bodies for them.
# Included from cmake/SfcopLifter.cmake after the exe lift; tools/objlift/README.md has the detail.
#
# For every claimed unit, at build time (tools/objlift/objlift.sh):
#   1. VC6 compiles it in the toolchain container, exactly as the harness does for the byte check;
#   2. tools/objlift/objlift.py places its object's sections, resolves its relocations against the
#      exe's symbols and lifts its code with pcrecomp in the lifter container, against this lift.
# Then, in this file:
#   - sfcop_objlift, an OBJECT library, holds the lifted bodies (one objlift_code_<UNIT>.c per
#     claimed unit), the override table, the data image of any section placed in the reserved
#     guest range, and the exe lift's dispatch table with the new bodies merged in (it replaces
#     recomp_dispatch.c in sfcop_lifted);
#   - every recomp_NNNN.c of sfcop_lifted is compiled with its own objlift_weak_NNNN.h, which
#     declares weak the exe-lift bodies of that file the claimed functions replace, so the strong
#     lifted-object bodies win for direct calls, the dispatch table and every guest vtable slot;
#   - objlift.py rewrites a file only when its content changed and compile.py recompiles a unit
#     only when its source or a header changed, so a change to one unit recompiles that unit's
#     code file, the exe-lift files that hold its claimed functions, and the table and dispatch;
#   - the objects are linked through sfcop_lifted's interface (an archive member would never be
#     loaded to replace a symbol that is already defined, even weakly), and override.cpp, the
#     empty guest-mode table, leaves sfcop_src.
# When the manifest claims nothing for VC6, or SFCOP_OBJLIFT is OFF, nothing is added and every
# function stays the exe lift's.
include_guard(GLOBAL)

option(SFCOP_OBJLIFT
    "Lift the VC6 objects of the units the decompilation's op/manifest.tsv claims into sfcop" ON)

set(_sfcop_manifest "${SFCOP_DECOMP_SOURCE}/manifest.tsv")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_sfcop_manifest}")
file(STRINGS "${_sfcop_manifest}" _sfcop_rows)
set(_sfcop_claimed 0)
set(_sfcop_units "")
set(_sfcop_unit_code "")
foreach(_row IN LISTS _sfcop_rows)
    string(REPLACE "\t" ";" _f "${_row}")
    list(LENGTH _f _n)
    if(_n LESS 5)
        continue()
    endif()
    list(GET _f 3 _unit)
    list(GET _f 4 _modes)
    string(REPLACE " " ";" _modes "${_modes}")
    if(NOT "vc6" IN_LIST _modes)
        continue()
    endif()
    math(EXPR _sfcop_claimed "${_sfcop_claimed} + 1")
    list(APPEND _sfcop_units "${SFCOP_DECOMP_SOURCE}/${_unit}")
    # objlift.py's code_file_name: the unit's path without its extension, as a C identifier.
    string(REGEX REPLACE "\\.[^./]*$" "" _stem "${_unit}")
    string(MAKE_C_IDENTIFIER "${_stem}" _stem)
    list(APPEND _sfcop_unit_code "objlift_code_${_stem}.c")
endforeach()
list(REMOVE_DUPLICATES _sfcop_units)
list(REMOVE_DUPLICATES _sfcop_unit_code)

if(NOT SFCOP_OBJLIFT OR _sfcop_claimed EQUAL 0)
    if(NOT SFCOP_OBJLIFT)
        message(STATUS "sfcop: SFCOP_OBJLIFT is OFF: every function is the exe lift's")
    endif()
    return()
endif()
# Guest mode (the spike's fallback, src/app/guest/override.h) defines the same entry points.
if("$CACHE{SFCOP_GUEST_MODE}")
    message(FATAL_ERROR "SFCOP_GUEST_MODE and SFCOP_OBJLIFT both replace the claimed functions' "
        "bodies: configure with one of them OFF")
endif()

set(SFCOP_OBJLIFT_DIR "${CMAKE_BINARY_DIR}/objlift")
# One weak header per exe-lift file: recomp_0012.c is compiled with objlift_weak_0012.h.
set(_sfcop_weak_headers "")
foreach(_c IN LISTS _sfcop_lifted_c)
    get_filename_component(_n "${_c}" NAME_WE)
    string(REPLACE "recomp_" "objlift_weak_" _n "${_n}")
    list(APPEND _sfcop_weak_headers "${SFCOP_OBJLIFT_DIR}/${_n}.h")
endforeach()
list(TRANSFORM _sfcop_unit_code PREPEND "${SFCOP_OBJLIFT_DIR}/" OUTPUT_VARIABLE _sfcop_unit_code)
set(_sfcop_objlift_files objlift_funcs.h objlift_table.c objlift_dispatch.c objlift_report.txt)
list(TRANSFORM _sfcop_objlift_files PREPEND "${SFCOP_OBJLIFT_DIR}/" OUTPUT_VARIABLE _sfcop_objlift_out)
# Byproducts: with Ninja the step restats them, so a file objlift.py left untouched rebuilds nothing.
list(APPEND _sfcop_objlift_out ${_sfcop_unit_code} ${_sfcop_weak_headers})

# What the lift reads: the decompilation's op/ (a header change can change any unit's code), its
# manifest, compiler profiles and symbol evidence, the exe and its lift, the tools themselves, and
# the patched pcrecomp (its stamp changes when a patch does: the patches change pcrecomp's lift tools).
file(GLOB_RECURSE _sfcop_objlift_deps CONFIGURE_DEPENDS
    "${SFCOP_DECOMP_SOURCE}/*.h" "${SFCOP_DECOMP_SOURCE}/*.cpp"
    "${CMAKE_SOURCE_DIR}/tools/objlift/*.py" "${SFCOP_DECOMP_DIR}/harness/lib/*.py"
    "${SFCOP_DECOMP_DIR}/analysis/results/*.tsv")
list(APPEND _sfcop_objlift_deps "${_sfcop_manifest}" "${SFCOP_DECOMP_DIR}/harness/units.json"
    "${SFCOP_DECOMP_DIR}/ghidra/exports/functions.csv" "${CMAKE_SOURCE_DIR}/tools/objlift/objlift.sh"
    "${CMAKE_SOURCE_DIR}/cmake/lifter/Dockerfile" "${_sfcop_exe}"
    "${_sfcop_gen}/recomp_funcs.h" "${_sfcop_gen}/recomp_dispatch.c" "${SFCOP_PCRECOMP_STAMP}")

list(LENGTH _sfcop_units _sfcop_nunits)
add_custom_command(
    OUTPUT "${SFCOP_OBJLIFT_DIR}/objlift.stamp"
    BYPRODUCTS ${_sfcop_objlift_out}
    COMMAND "${CMAKE_COMMAND}" -E env "PYTHON=${Python3_EXECUTABLE}"
            "SFCOP_DECOMP_DIR=${SFCOP_DECOMP_DIR}"
            sh "${CMAKE_SOURCE_DIR}/tools/objlift/objlift.sh"
            --manifest "${_sfcop_manifest}" --exe "${_sfcop_exe}" --gen "${_sfcop_gen}"
            --pcrecomp "${SFCOP_PCRECOMP_BUILD_DIR}" --out "${SFCOP_OBJLIFT_DIR}"
    DEPENDS ${_sfcop_objlift_deps}
    COMMENT "Lifting the VC6 objects of ${_sfcop_nunits} claimed unit(s) (${_sfcop_claimed} functions)"
    USES_TERMINAL
    VERBATIM)
add_custom_target(sfcop_objlift_gen DEPENDS "${SFCOP_OBJLIFT_DIR}/objlift.stamp")

add_library(sfcop_objlift OBJECT ${_sfcop_unit_code}
    "${SFCOP_OBJLIFT_DIR}/objlift_table.c" "${SFCOP_OBJLIFT_DIR}/objlift_dispatch.c")
target_include_directories(sfcop_objlift PRIVATE "${SFCOP_OBJLIFT_DIR}" ${SFCOP_RECOMP_INCLUDES}
    "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(sfcop_objlift PRIVATE ${SFCOP_RECOMP_DEFINITIONS})
# Generated code, compiled as sfcop_lifted's is.
target_compile_options(sfcop_objlift PRIVATE -w -O2 -fno-strict-aliasing -fwrapv)
add_dependencies(sfcop_objlift sfcop_objlift_gen)

# The exe lift's bodies for the claimed functions give way; its dispatch table is the merged one.
get_target_property(_sfcop_lifted_sources sfcop_lifted SOURCES)
list(REMOVE_ITEM _sfcop_lifted_sources "${_sfcop_gen}/recomp_dispatch.c")
set_property(TARGET sfcop_lifted PROPERTY SOURCES ${_sfcop_lifted_sources})
foreach(_c _h IN ZIP_LISTS _sfcop_lifted_c _sfcop_weak_headers)
    set_property(SOURCE "${_c}" APPEND PROPERTY COMPILE_OPTIONS -include "${_h}")
    set_property(SOURCE "${_c}" APPEND PROPERTY OBJECT_DEPENDS "${_h}")
endforeach()
add_dependencies(sfcop_lifted sfcop_objlift_gen sfcop_objlift)
target_link_libraries(sfcop_lifted INTERFACE $<TARGET_OBJECTS:sfcop_objlift>)

# override.cpp's empty table would answer for the lifted objects' table.
set(_sfcop_override "${CMAKE_SOURCE_DIR}/src/app/guest/override.cpp")
get_target_property(_sfcop_src_type sfcop_src TYPE)
if(NOT _sfcop_src_type STREQUAL "INTERFACE_LIBRARY")
    get_target_property(_sfcop_src_sources sfcop_src SOURCES)
    list(REMOVE_ITEM _sfcop_src_sources "${_sfcop_override}")
    set_property(TARGET sfcop_src PROPERTY SOURCES ${_sfcop_src_sources})
endif()

message(STATUS "sfcop: lifting the VC6 objects of ${_sfcop_nunits} claimed unit(s), "
    "${_sfcop_claimed} functions (tools/objlift)")
