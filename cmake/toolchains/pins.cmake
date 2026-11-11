# Shared by windows-x64.cmake and linux-x64.cmake: reads pins.env and finds the installed LLVM.
# CMake reads a toolchain file more than once (once per try_compile), so this file only sets
# variables. Where the tools are (README.md, "The runner"):
#   SFCOP_TOOLS_DIR   the tools folder setup.sh installed into
#   SFCOP_LLVM_DIR    overrides <tools>/llvm/<LLVM_VERSION>
include_guard(GLOBAL)

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/pins.env" _sfcop_pin_lines REGEX "^[A-Z0-9_]+=")
foreach(_sfcop_line IN LISTS _sfcop_pin_lines)
    string(REGEX MATCH "^([A-Z0-9_]+)=(.*)$" _ "${_sfcop_line}")
    set(SFCOP_PIN_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
endforeach()

# The tools folder is always named: the sysroot defaults below hang off it, so SFCOP_LLVM_DIR
# alone (one part somewhere else) does not stand in for it.
if(NOT DEFINED ENV{SFCOP_TOOLS_DIR})
    message(FATAL_ERROR "${SFCOP_PLATFORM}: SFCOP_TOOLS_DIR is not set. Set it to the folder "
        "`sh cmake/toolchains/setup.sh` installs into (cmake/toolchains/README.md, \"The runner\").")
endif()
set(SFCOP_TOOLS_DIR "$ENV{SFCOP_TOOLS_DIR}")
if(NOT IS_DIRECTORY "${SFCOP_TOOLS_DIR}")
    message(FATAL_ERROR "${SFCOP_PLATFORM}: the tools folder does not exist: SFCOP_TOOLS_DIR is set to "
        "${SFCOP_TOOLS_DIR}. Set it to the folder `sh cmake/toolchains/setup.sh` "
        "installs into (cmake/toolchains/README.md, \"The runner\").")
endif()
if(DEFINED ENV{SFCOP_LLVM_DIR})
    set(SFCOP_LLVM_DIR "$ENV{SFCOP_LLVM_DIR}")
else()
    set(SFCOP_LLVM_DIR "${SFCOP_TOOLS_DIR}/llvm/${SFCOP_PIN_LLVM_VERSION}")
endif()

# sfcop_require_tool(<path> <what>): stop the configure with the command that installs it.
function(sfcop_require_tool path what)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "${SFCOP_PLATFORM}: ${what} not found at ${path}. Install the pinned "
            "toolchains with `sh cmake/toolchains/setup.sh`, or point SFCOP_TOOLS_DIR at them "
            "(cmake/toolchains/README.md).")
    endif()
endfunction()

sfcop_require_tool("${SFCOP_LLVM_DIR}/bin/clang" "LLVM ${SFCOP_PIN_LLVM_VERSION}")
set(CMAKE_C_COMPILER "${SFCOP_LLVM_DIR}/bin/clang")
set(CMAKE_CXX_COMPILER "${SFCOP_LLVM_DIR}/bin/clang++")
set(CMAKE_AR "${SFCOP_LLVM_DIR}/bin/llvm-ar" CACHE FILEPATH "Archiver")
set(CMAKE_RANLIB "${SFCOP_LLVM_DIR}/bin/llvm-ranlib" CACHE FILEPATH "Ranlib")

# Nothing from the Mac's own include or library folders.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
