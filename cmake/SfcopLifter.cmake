# The lifted game (DESIGN §7.2, §7.4): StarFleetOP.exe lifted to C at build time, from the
# install, into ${CMAKE_BINARY_DIR}/lifted. Nothing generated here is committed.
#
# Inputs, each a cache variable with a default from harness/lib/paths.py (cmake/lifter/locate.py):
#   SFCOP_INSTALL_DIR    the original game's install folder; its StarFleetOP.exe is hash-checked
#                        against analysis/results/retail_hashes.sha256 before every lift
#   SFCOP_PCRECOMP_DIR   a pcrecomp clone at the pinned revision with cmake/lifter/patches applied
#                        (cmake/lifter/setup.sh makes one)
# When either is missing, configure still succeeds and says what is missing; building `sfcop`
# then fails with the same message.
#
# Provides:
#   SFCOP_LIFT_READY         ON when both inputs are present
#   SFCOP_LIFT_MISSING       what is missing, as one sentence per item
#   sfcop_lifted             STATIC library of the lifted functions (when ready)
#   sfcop_win32hle           OBJECT library of pcrecomp's win32hle runtime (when ready)
#   sfcop_gameroot           target: a writable copy of the install in ${CMAKE_BINARY_DIR}/gameroot,
#                            for the boot probes and proofs (tools/boot_probe.sh clones one per run);
#                            the guest host never writes the install
include_guard(GLOBAL)

find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/cmake/lifter/locate.py" "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE _sfcop_located RESULT_VARIABLE _sfcop_rc ERROR_VARIABLE _sfcop_err)
if(NOT _sfcop_rc EQUAL 0)
    message(FATAL_ERROR "cmake/lifter/locate.py failed: ${_sfcop_err}")
endif()
cmake_language(EVAL CODE "${_sfcop_located}")

get_filename_component(_sfcop_default_install "${SFCOP_FOUND_EXE}" DIRECTORY)
set(SFCOP_INSTALL_DIR "${_sfcop_default_install}" CACHE PATH
    "The original game's install folder (holds StarFleetOP.exe)")
set(SFCOP_PCRECOMP_DIR "${SFCOP_FOUND_WORK}/lifter-hle/pcrecomp" CACHE PATH
    "pcrecomp clone at the pinned revision with cmake/lifter/patches applied")
set(SFCOP_EXE_SHA256 "${SFCOP_FOUND_EXE_SHA256}")

# Number of lifted .c files: fixed, so the build knows their names before the lift runs.
set(SFCOP_LIFT_FILES 64)
# Functions whose entry calls the guest host's sfcop_mark(va), which logs "[mark] reached ...":
# WinMain, the function that shows the CD-key prompt, the main menu (the entry of
# tState_Namespace_Setup::tState_SelectGame, the "Select Game" screen) and Setup (the entry of
# tState_Namespace_Setup::tState_Setup, the Single Player screen) (src/app/guest/GuestGame.cpp).
set(SFCOP_MARK_WINMAIN 0x0069A840)
set(SFCOP_MARK_CDKEY_PROMPT 0x00484102)
set(SFCOP_MARK_MENU 0x0047B8F7)
set(SFCOP_MARK_SETUP 0x0047BC9C)
set(SFCOP_LIFT_EXTRA_MARKS "" CACHE STRING
    "More guest addresses to mark, comma-separated (diagnostics: each logs when first reached)")
set(SFCOP_LIFT_MARKS
    "${SFCOP_MARK_WINMAIN},${SFCOP_MARK_CDKEY_PROMPT},${SFCOP_MARK_MENU},${SFCOP_MARK_SETUP}")
if(SFCOP_LIFT_EXTRA_MARKS)
    string(APPEND SFCOP_LIFT_MARKS ",${SFCOP_LIFT_EXTRA_MARKS}")
endif()

set(SFCOP_LIFT_MISSING "")
set(_sfcop_exe "${SFCOP_INSTALL_DIR}/StarFleetOP.exe")
if(NOT EXISTS "${_sfcop_exe}")
    string(APPEND SFCOP_LIFT_MISSING
        "No StarFleetOP.exe in SFCOP_INSTALL_DIR (\"${SFCOP_INSTALL_DIR}\"): configure with "
        "-DSFCOP_INSTALL_DIR=<the install folder>. ")
endif()
if(SFCOP_EXE_SHA256 STREQUAL "")
    string(APPEND SFCOP_LIFT_MISSING
        "analysis/results/retail_hashes.sha256 has no line for the exe. ")
endif()
set(_sfcop_win32hle "${SFCOP_PCRECOMP_DIR}/runtime/win32hle")
if(NOT EXISTS "${SFCOP_PCRECOMP_DIR}/tools/lift/lift32.py" OR NOT EXISTS "${_sfcop_win32hle}/win32hle.h")
    string(APPEND SFCOP_LIFT_MISSING
        "No pcrecomp clone at SFCOP_PCRECOMP_DIR (\"${SFCOP_PCRECOMP_DIR}\"): run "
        "cmake/lifter/setup.sh <dir> and configure with -DSFCOP_PCRECOMP_DIR=<dir>. ")
else()
    find_package(Git REQUIRED)
    foreach(_p IN ITEMS pcrecomp-tib-hook pcrecomp-kernel32-unnamed-objects
                        pcrecomp-manual-lookup-hook pcrecomp-wsock32-select-accept
                        pcrecomp-eh-landing-pads pcrecomp-mach-depth pcrecomp-div-fault
                        pcrecomp-quantum pcrecomp-win32-posix pcrecomp-win32-paths
                        pcrecomp-win32-kernel32 pcrecomp-win32-cpuid)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${SFCOP_PCRECOMP_DIR}" apply --reverse --check
                    "${CMAKE_SOURCE_DIR}/cmake/lifter/patches/${_p}.patch"
            RESULT_VARIABLE _sfcop_rc OUTPUT_QUIET ERROR_QUIET)
        if(NOT _sfcop_rc EQUAL 0)
            string(APPEND SFCOP_LIFT_MISSING
                "The pcrecomp clone lacks ${_p}.patch: run cmake/lifter/setup.sh \"${SFCOP_PCRECOMP_DIR}\". ")
        endif()
    endforeach()
endif()

if(SFCOP_LIFT_MISSING STREQUAL "")
    set(SFCOP_LIFT_READY ON)
    message(STATUS "sfcop: lifting ${_sfcop_exe} with pcrecomp at ${SFCOP_PCRECOMP_DIR}")
else()
    set(SFCOP_LIFT_READY OFF)
    message(WARNING "sfcop cannot be built on this machine: ${SFCOP_LIFT_MISSING}")
    return()
endif()

# --- The lift ------------------------------------------------------------------------------------
set(SFCOP_LIFTED_DIR "${CMAKE_BINARY_DIR}/lifted")
set(_sfcop_gen "${SFCOP_LIFTED_DIR}/out/gen")
set(_sfcop_lifted_c "")
math(EXPR _sfcop_last "${SFCOP_LIFT_FILES} - 1")
foreach(_i RANGE ${_sfcop_last})
    string(LENGTH "${_i}" _len)
    math(EXPR _pad "4 - ${_len}")
    string(REPEAT "0" ${_pad} _zeros)
    list(APPEND _sfcop_lifted_c "${_sfcop_gen}/recomp_${_zeros}${_i}.c")
endforeach()
add_custom_command(
    OUTPUT ${_sfcop_lifted_c} "${_sfcop_gen}/recomp_dispatch.c" "${_sfcop_gen}/recomp_funcs.h"
           "${_sfcop_gen}/recomp_eh_table.c"
    COMMAND sh "${CMAKE_SOURCE_DIR}/cmake/lifter/lift.sh"
            --exe "${_sfcop_exe}" --sha256 "${SFCOP_EXE_SHA256}" --pcrecomp "${SFCOP_PCRECOMP_DIR}"
            --out "${SFCOP_LIFTED_DIR}" --files ${SFCOP_LIFT_FILES} --marks "${SFCOP_LIFT_MARKS}"
    DEPENDS "${_sfcop_exe}" "${CMAKE_SOURCE_DIR}/cmake/lifter/lift.sh"
            "${CMAKE_SOURCE_DIR}/cmake/lifter/run_lift.py" "${CMAKE_SOURCE_DIR}/cmake/lifter/eh_frames.py"
            "${CMAKE_SOURCE_DIR}/cmake/lifter/Dockerfile"
    COMMENT "Lifting StarFleetOP.exe (rtti, disasm, lift: about 6 minutes the first time)"
    USES_TERMINAL
    VERBATIM)

set(SFCOP_RECOMP_DEFINITIONS RECOMP_RETADDR=0u RECOMP_LOCAL_REGS RECOMP_MEM_BASE=0x100000000000ull)
set(SFCOP_RECOMP_INCLUDES "${_sfcop_gen}" "${_sfcop_win32hle}" "${SFCOP_PCRECOMP_DIR}/runtime/recomp32")

# recomp_eh_table.c: every catch and __except continuation with the function that owns it, which
# the guest host checks each resume against (src/app/guest/).
add_library(sfcop_lifted STATIC ${_sfcop_lifted_c} "${_sfcop_gen}/recomp_dispatch.c"
    "${_sfcop_gen}/recomp_eh_table.c")
target_include_directories(sfcop_lifted PRIVATE ${SFCOP_RECOMP_INCLUDES})
target_compile_definitions(sfcop_lifted PRIVATE ${SFCOP_RECOMP_DEFINITIONS})
# Generated code: no warnings, the spike's optimisation and its integer semantics.
target_compile_options(sfcop_lifted PRIVATE -w -O2 -fno-strict-aliasing -fwrapv)

# --- pcrecomp's win32hle runtime (third-party, MIT; compiled as it is) ------------------------------
file(GLOB _sfcop_win32hle_c CONFIGURE_DEPENDS "${_sfcop_win32hle}/*.c")
list(FILTER _sfcop_win32hle_c EXCLUDE REGEX "_selftest\\.c$")
# TEMPORARY: win32hle is written against SDL2 (the pinned sdl2-compat, third_party/); it leaves
# at gate G1, with the spike's stubs, once every row it still answers has an owner.
add_library(sfcop_win32hle OBJECT ${_sfcop_win32hle_c})
target_include_directories(sfcop_win32hle BEFORE PRIVATE "${CMAKE_SOURCE_DIR}/src/app/boot/include")
target_include_directories(sfcop_win32hle PRIVATE ${SFCOP_RECOMP_INCLUDES})
target_compile_definitions(sfcop_win32hle PRIVATE ${SFCOP_RECOMP_DEFINITIONS})
target_compile_options(sfcop_win32hle PRIVATE -w -fno-strict-aliasing -fwrapv)
target_link_libraries(sfcop_win32hle PUBLIC SDL2::SDL2)
# The runtime is written against POSIX. On Windows its win32/ layer stands in
# (pcrecomp-win32-posix.patch): posix.h first in every file, POSIX header names on the include path
# that bring it in, the calls on the Windows API in posix.c, sockets on Winsock. Elsewhere this does
# nothing. For every target that compiles runtime files.
function(sfcop_win32hle_posix target)
    if(WIN32)
        target_sources(${target} PRIVATE "${_sfcop_win32hle}/win32/posix.c")
        target_include_directories(${target} BEFORE PRIVATE "${_sfcop_win32hle}/win32/include")
        target_compile_options(${target} PRIVATE "SHELL:-include \"${_sfcop_win32hle}/win32/posix.h\"")
        target_link_libraries(${target} PUBLIC ws2_32)
    endif()
endfunction()
sfcop_win32hle_posix(sfcop_win32hle)

# --- A writable copy of the install for the boot probes and the proofs ------------------------------
set(SFCOP_GAMEROOT "${CMAKE_BINARY_DIR}/gameroot")
add_custom_command(
    OUTPUT "${SFCOP_GAMEROOT}/.sfcop-copy"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${SFCOP_GAMEROOT}"
    # A clone on APFS (no data copied), a plain copy elsewhere.
    COMMAND sh -c "cp -Rc \"$1\" \"$2\" 2>/dev/null || cp -R \"$1\" \"$2\"" sh "${SFCOP_INSTALL_DIR}" "${SFCOP_GAMEROOT}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${SFCOP_GAMEROOT}/.sfcop-copy"
    DEPENDS "${_sfcop_exe}"
    COMMENT "Copying the install to ${SFCOP_GAMEROOT} (a writable copy for the boot probes)"
    VERBATIM)
add_custom_target(sfcop_gameroot DEPENDS "${SFCOP_GAMEROOT}/.sfcop-copy")

# The claimed units' own VC6 objects, lifted, in place of their lifted-exe bodies.
include("${CMAKE_SOURCE_DIR}/cmake/lifter/objlift.cmake")
