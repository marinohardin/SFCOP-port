# The object lift (DESIGN §6.3, §7.2): the functions game/manifest.tsv claims for VC6 enter the
# native build as their own VC6 objects, lifted, in place of the exe lift's bodies for them.
# Included from cmake/SfcopLifter.cmake after the exe lift; tools/objlift/README.md has the detail.
#
# For every claimed unit, at build time (tools/objlift/objlift.sh):
#   1. VC6 compiles it in the toolchain container, exactly as the harness does for the byte check;
#   2. tools/objlift/objlift.py places its object's sections, resolves its relocations against the
#      exe's symbols and lifts its code with pcrecomp in the lifter container, against this lift.
# Then, in this file:
#   - sfcop_objlift, an OBJECT library, holds the lifted bodies, the override table, the data image
#     of any section placed in the reserved guest range, and the exe lift's dispatch table with the
#     new bodies merged in (it replaces recomp_dispatch.c in sfcop_lifted);
#   - every file of sfcop_lifted is compiled with objlift_weak.h, which declares the exe-lift bodies
#     of the claimed functions weak, so the strong lifted-object bodies win for direct calls, the
#     dispatch table and every guest vtable slot;
#   - the objects are linked through sfcop_lifted's interface (an archive member would never be
#     loaded to replace a symbol that is already defined, even weakly), and override.cpp, the
#     empty guest-mode table, leaves sfcop_src.
# When the manifest claims nothing for VC6, or SFCOP_OBJLIFT is OFF, nothing is added and every
# function stays the exe lift's.
include_guard(GLOBAL)

option(SFCOP_OBJLIFT "Lift the VC6 objects of the units game/manifest.tsv claims into sfcop" ON)

set(_sfcop_manifest "${CMAKE_SOURCE_DIR}/game/manifest.tsv")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_sfcop_manifest}")
file(STRINGS "${_sfcop_manifest}" _sfcop_rows)
set(_sfcop_claimed 0)
set(_sfcop_units "")
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
    list(APPEND _sfcop_units "${CMAKE_SOURCE_DIR}/game/${_unit}")
endforeach()
list(REMOVE_DUPLICATES _sfcop_units)

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
set(_sfcop_objlift_files objlift_code.c objlift_funcs.h objlift_table.c objlift_dispatch.c
    objlift_weak.h objlift_report.txt)
list(TRANSFORM _sfcop_objlift_files PREPEND "${SFCOP_OBJLIFT_DIR}/" OUTPUT_VARIABLE _sfcop_objlift_out)

# What the lift reads: game/ (a header change can change any unit's code), the manifest, the
# compiler profiles, the symbol evidence, the exe and its lift, and the tools themselves.
file(GLOB_RECURSE _sfcop_objlift_deps CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/game/*.h" "${CMAKE_SOURCE_DIR}/game/*.cpp"
    "${CMAKE_SOURCE_DIR}/tools/objlift/*.py" "${CMAKE_SOURCE_DIR}/harness/lib/*.py"
    "${CMAKE_SOURCE_DIR}/analysis/results/*.tsv")
list(APPEND _sfcop_objlift_deps "${_sfcop_manifest}" "${CMAKE_SOURCE_DIR}/harness/units.json"
    "${CMAKE_SOURCE_DIR}/ghidra/exports/functions.csv" "${CMAKE_SOURCE_DIR}/tools/objlift/objlift.sh"
    "${CMAKE_SOURCE_DIR}/cmake/lifter/Dockerfile" "${_sfcop_exe}"
    "${_sfcop_gen}/recomp_funcs.h" "${_sfcop_gen}/recomp_dispatch.c")

list(LENGTH _sfcop_units _sfcop_nunits)
add_custom_command(
    OUTPUT "${SFCOP_OBJLIFT_DIR}/objlift.stamp"
    BYPRODUCTS ${_sfcop_objlift_out}
    COMMAND "${CMAKE_COMMAND}" -E env "PYTHON=${Python3_EXECUTABLE}"
            sh "${CMAKE_SOURCE_DIR}/tools/objlift/objlift.sh"
            --manifest "${_sfcop_manifest}" --exe "${_sfcop_exe}" --gen "${_sfcop_gen}"
            --pcrecomp "${SFCOP_PCRECOMP_DIR}" --out "${SFCOP_OBJLIFT_DIR}"
    DEPENDS ${_sfcop_objlift_deps}
    COMMENT "Lifting the VC6 objects of ${_sfcop_nunits} claimed unit(s) (${_sfcop_claimed} functions)"
    USES_TERMINAL
    VERBATIM)
add_custom_target(sfcop_objlift_gen DEPENDS "${SFCOP_OBJLIFT_DIR}/objlift.stamp")

add_library(sfcop_objlift OBJECT "${SFCOP_OBJLIFT_DIR}/objlift_code.c"
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
target_compile_options(sfcop_lifted PRIVATE -include "${SFCOP_OBJLIFT_DIR}/objlift_weak.h")
set_source_files_properties(${_sfcop_lifted_sources} PROPERTIES
    OBJECT_DEPENDS "${SFCOP_OBJLIFT_DIR}/objlift_weak.h")
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
