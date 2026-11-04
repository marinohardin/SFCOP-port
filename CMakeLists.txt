# The native build (DESIGN §7.2): the lifted game plus the platform layer, for macOS arm64 now
# and Windows x64 and Linux x64 later.
#
#   cmake --preset macos-arm64
#   cmake --build --preset macos-arm64 -j4
#   ./build/macos-arm64/sfcop --headless --watchdog 30
#   ctest --preset macos-arm64
#
# The 32-bit Windows build (the delinked original relinked with VC6) is not built here; it lives
# in toolchain/ and harness/.
cmake_minimum_required(VERSION 3.28)

# No language yet: a preset whose cross toolchain is still a stub configures without a compiler.
project(sfcop
    VERSION 0.0.0
    DESCRIPTION "Star Trek Starfleet Command II: Orion Pirates 2.5.5.2, native"
    LANGUAGES NONE)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Keep build trees out of `git status` without a .gitignore entry.
file(WRITE "${CMAKE_BINARY_DIR}/.gitignore" "*\n")

if(SFCOP_TOOLCHAIN_STUB)
    message(STATUS "sfcop: ${SFCOP_PLATFORM} configures only; nothing is built. Missing: "
                   "${SFCOP_TOOLCHAIN_MISSING}")
    return()
endif()

enable_language(C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

include(SfcopWarnings)
# OFF builds only src/ and the tests: no lift, no sfcop, no boot platform. CI turns it off on a
# runner that has neither the install nor the pcrecomp clone.
option(SFCOP_LIFT "Lift the original and build the sfcop client (needs the install and pcrecomp)" ON)
if(APPLE AND SFCOP_LIFT)
    # sdl2-compat (third_party/) has Objective-C sources; a language must be enabled here, not
    # only in the subdirectory that uses it.
    enable_language(OBJC)
endif()
enable_testing()
add_subdirectory(third_party)
# sfcop_src (the new code under src/) and sfcop_tests (doctest, CTest).
include(SfcopSources)
if(NOT SFCOP_LIFT)
    message(STATUS "skip: sfcop and the lift (SFCOP_LIFT is OFF); building src/ and the tests only")
    return()
endif()
include(SfcopLifter)

# --- sfcop: the native client --------------------------------------------------------------------
if(NOT SFCOP_LIFT_READY)
    # Configure succeeds so that the tests and the other presets still work; building the client
    # says what is missing.
    add_custom_target(sfcop ALL
        COMMAND "${CMAKE_COMMAND}" -E echo "sfcop cannot be built: ${SFCOP_LIFT_MISSING}"
        COMMAND "${CMAKE_COMMAND}" -E false
        VERBATIM)
    return()
endif()

configure_file(cmake/sfcop_build_config.h.in "${CMAKE_BINARY_DIR}/generated/sfcop_build_config.h" @ONLY)

# TEMPORARY: the boot platform, the P1-2 spike's host and HLE modules, until Wave 1 (G1: "no
# spike path linked").
file(GLOB SFCOP_BOOT_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/app/boot/*.c")
add_library(sfcop_boot OBJECT ${SFCOP_BOOT_SOURCES})
target_include_directories(sfcop_boot BEFORE PRIVATE "${CMAKE_SOURCE_DIR}/src/app/boot/include")
target_include_directories(sfcop_boot PRIVATE "${CMAKE_SOURCE_DIR}/src/app/boot" ${SFCOP_RECOMP_INCLUDES})
target_compile_definitions(sfcop_boot PRIVATE ${SFCOP_RECOMP_DEFINITIONS}
    SFCOP_MARK_WINMAIN=${SFCOP_MARK_WINMAIN}u SFCOP_MARK_CDKEY_PROMPT=${SFCOP_MARK_CDKEY_PROMPT}u)
target_compile_options(sfcop_boot PRIVATE -fno-strict-aliasing -fwrapv)
sfcop_strict_warnings(sfcop_boot)
target_link_libraries(sfcop_boot PRIVATE SDL2::SDL2)
# The lifted code's headers come from the lift.
add_dependencies(sfcop_boot sfcop_lifted)

add_executable(sfcop src/app/main.cpp)
target_include_directories(sfcop PRIVATE "${CMAKE_SOURCE_DIR}/src/app" "${CMAKE_BINARY_DIR}/generated")
sfcop_strict_warnings(sfcop)
# The boot platform reaches SDL3 only through sdl2-compat, which loads libSDL3.dylib from beside
# sfcop; both libraries are written there (third_party/CMakeLists.txt).
target_link_libraries(sfcop PRIVATE sfcop_src sfcop_boot sfcop_win32hle sfcop_lifted
    SDL2::SDL2 pthread m)
add_dependencies(sfcop SDL3-shared)
add_dependencies(sfcop sfcop_gameroot)
