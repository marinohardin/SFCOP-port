# The native build (DESIGN §7.2): the lifted game plus the platform layer, for macOS arm64 now
# and Windows x64 and Linux x64 later.
#
#   cmake --preset macos-arm64
#   cmake --build --preset macos-arm64 -j4
#   ./build/macos-arm64/sfcop --headless --watchdog 30
#   ctest --preset macos-arm64
#
# The 32-bit Windows build (the delinked original relinked with VC6) is not built here; it lives
# in the decompilation (the decomp submodule, SFCOP_DECOMP_DIR: its toolchain/ and harness/).
cmake_minimum_required(VERSION 3.28)

# No language yet: a preset whose cross toolchain is still a stub configures without a compiler.
project(sfcop
    VERSION 0.0.0
    DESCRIPTION "Star Trek Starfleet Command II: Orion Pirates 2.5.5.2, native"
    LANGUAGES NONE)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Keep build trees out of `git status` without a .gitignore entry.
file(WRITE "${CMAKE_BINARY_DIR}/.gitignore" "*\n")

# The decompilation (SFCOP_DECOMP_DIR, the decomp submodule): every path into it goes through here.
include(SfcopDecomp)

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
# A Windows package build's binaries, SDL's included, name their .pdb by file name, not by its
# path in the build folder (packaging/check_paths.py). SFCOP_PACKAGE_BUILD is declared below;
# its value arrives here through -D (scripts/dist.sh), before the option() line runs.
if(WIN32 AND SFCOP_PACKAGE_BUILD)
    add_link_options("LINKER:/PDBALTPATH:%_PDB%")
endif()
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

# A package build (scripts/dist.sh) carries no build-machine folder: the generated configuration's
# strings are left empty, and the packaged client reads the hash list it carries and asks for the
# install (src/app/Bundle.h, packaging/check_paths.py).
option(SFCOP_PACKAGE_BUILD "Compile no build-machine folder into the client (release packages)" OFF)
if(SFCOP_PACKAGE_BUILD)
    set(SFCOP_CONFIG_INSTALL_DIR "")
    set(SFCOP_CONFIG_GAME_DIR "")
    set(SFCOP_CONFIG_HASH_LIST "")
    set(SFCOP_CONFIG_SOURCE_DIR "")
else()
    set(SFCOP_CONFIG_INSTALL_DIR "${SFCOP_INSTALL_DIR}")
    set(SFCOP_CONFIG_GAME_DIR "${SFCOP_GAMEROOT}")
    set(SFCOP_CONFIG_HASH_LIST "${SFCOP_DECOMP_DIR}/analysis/results/retail_hashes.sha256")
    set(SFCOP_CONFIG_SOURCE_DIR "${CMAKE_SOURCE_DIR}")
endif()
configure_file(cmake/sfcop_build_config.h.in "${CMAKE_BINARY_DIR}/generated/sfcop_build_config.h" @ONLY)

# The registration of the HLE module tables, generated from the shim files the build found
# (cmake/SfcopSources.cmake collects them as SFCOP_GUEST_SOURCES); empty when there are none.
set(SFCOP_HLE_MODULE_DECLS "")
set(SFCOP_HLE_MODULE_REGISTRATIONS "")
foreach(_sfcop_guest IN LISTS SFCOP_GUEST_SOURCES)
    get_filename_component(_sfcop_stem "${_sfcop_guest}" NAME_WE)
    string(REGEX REPLACE "Guest$" "" _sfcop_module "${_sfcop_stem}")
    string(TOLOWER "${_sfcop_module}" _sfcop_module)
    string(APPEND SFCOP_HLE_MODULE_DECLS "extern const win32hle_shim sfcop_hle_${_sfcop_module}[];\n")
    string(APPEND SFCOP_HLE_MODULE_REGISTRATIONS "    win32hle_register(sfcop_hle_${_sfcop_module});\n")
endforeach()
configure_file(cmake/sfcop_hle_modules.c.in "${CMAKE_BINARY_DIR}/generated/sfcop_hle_modules.c" @ONLY)

# The guest host (src/app/guest/): runs the lifted game on the HLE modules and the components.
# Built only with the lift; cmake/SfcopSources.cmake keeps it out of sfcop_src and the tests.
# SFCOP_GUEST_HOST_SOURCES is the part any lifted program uses (the guest tests link it too);
# GuestGame.cpp and B9's override.cpp are the original's own.
set(SFCOP_GUEST_HOST_SOURCES
    "${CMAKE_SOURCE_DIR}/src/app/guest/GuestHost.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/guest/GuestExceptions.cpp"
    "${CMAKE_BINARY_DIR}/generated/sfcop_hle_modules.c")
add_library(sfcop_guest OBJECT ${SFCOP_GUEST_HOST_SOURCES}
    "${CMAKE_SOURCE_DIR}/src/app/guest/GuestGame.cpp")
if(NOT TARGET sfcop_objlift)
    # The object lift (cmake/lifter/objlift.cmake) defines the override table and the data image
    # loader itself; override.cpp, the empty table, serves only when it claims nothing.
    target_sources(sfcop_guest PRIVATE "${CMAKE_SOURCE_DIR}/src/app/guest/override.cpp")
endif()
target_include_directories(sfcop_guest PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/src/app/guest"
    ${SFCOP_RECOMP_INCLUDES})
target_compile_definitions(sfcop_guest PRIVATE ${SFCOP_RECOMP_DEFINITIONS}
    SFCOP_MARK_WINMAIN=${SFCOP_MARK_WINMAIN}u SFCOP_MARK_CDKEY_PROMPT=${SFCOP_MARK_CDKEY_PROMPT}u
    SFCOP_MARK_MENU=${SFCOP_MARK_MENU}u SFCOP_MARK_SETUP=${SFCOP_MARK_SETUP}u)
target_compile_options(sfcop_guest PRIVATE -fno-strict-aliasing -fwrapv)
sfcop_strict_warnings(sfcop_guest)
# The lifted code's headers come from the lift.
add_dependencies(sfcop_guest sfcop_lifted)

# TEMPORARY: the P1-2 spike's stubs for rows no module owns yet (src/app/boot/misc_stubs.c and the
# helpers it calls); the guest host answers an import from them only when no module does. Goes at
# gate G1 with src/app/boot/.
add_library(sfcop_spike_unowned OBJECT "${CMAKE_SOURCE_DIR}/src/app/boot/misc_stubs.c"
    "${CMAKE_SOURCE_DIR}/src/app/boot/hle_util.c")
target_include_directories(sfcop_spike_unowned PRIVATE "${CMAKE_SOURCE_DIR}/src/app/boot"
    ${SFCOP_RECOMP_INCLUDES})
target_compile_definitions(sfcop_spike_unowned PRIVATE ${SFCOP_RECOMP_DEFINITIONS})
target_compile_options(sfcop_spike_unowned PRIVATE -fno-strict-aliasing -fwrapv)
sfcop_strict_warnings(sfcop_spike_unowned)
add_dependencies(sfcop_spike_unowned sfcop_lifted)

# The HLE modules' guest-facing shims (src/hle/<module>/*Guest.c|cpp, src/hle/README.md): they read
# the guest stack through the lift's headers, so they are built only with the lift.
if(SFCOP_GUEST_SOURCES)
    add_library(sfcop_hle_guest OBJECT ${SFCOP_GUEST_SOURCES})
    target_include_directories(sfcop_hle_guest PRIVATE "${CMAKE_SOURCE_DIR}/src" ${SFCOP_RECOMP_INCLUDES})
    target_compile_definitions(sfcop_hle_guest PRIVATE ${SFCOP_RECOMP_DEFINITIONS})
    target_compile_options(sfcop_hle_guest PRIVATE -fno-strict-aliasing -fwrapv)
    sfcop_strict_warnings(sfcop_hle_guest)
    add_dependencies(sfcop_hle_guest sfcop_lifted)
endif()

add_executable(sfcop src/app/main.cpp)
target_include_directories(sfcop PRIVATE "${CMAKE_SOURCE_DIR}/src/app" "${CMAKE_BINARY_DIR}/generated")
sfcop_strict_warnings(sfcop)
# pcrecomp's win32hle (the rows no module owns yet) reaches SDL3 only through sdl2-compat, which
# loads libSDL3.dylib from beside sfcop; both libraries are written there (third_party/CMakeLists.txt).
# On Windows SDL3-static comes before sdl2-compat's import library, or lld-link binds the platform
# layer's SDL3 calls that share an SDL2 name (SDL_GetWindowFlags...) to sdl2-compat.
target_link_libraries(sfcop PRIVATE sfcop_guest sfcop_spike_unowned sfcop_src sfcop_win32hle
    sfcop_lifted $<$<PLATFORM_ID:Windows>:SDL3::SDL3-static> SDL2::SDL2
    $<$<NOT:$<PLATFORM_ID:Windows>>:pthread> $<$<NOT:$<PLATFORM_ID:Windows>>:m>)
if(WIN32)
    # 8 MB, not Windows' 1 MB default: guest fault handlers run inside the vectored handler on this stack.
    target_link_options(sfcop PRIVATE "LINKER:/STACK:8388608")
endif()
if(TARGET sfcop_hle_guest)
    target_link_libraries(sfcop PRIVATE sfcop_hle_guest)
endif()
add_dependencies(sfcop SDL3-shared)
add_dependencies(sfcop sfcop_gameroot)
# SFCOP.app on macOS (cmake/SfcopBundle.cmake): the client, its libraries and the hash list.
include(SfcopBundle)

# --- The guest tests (src/app/guest/README.md, "Tests") ----------------------------------------------
# The guest host running a lifted test program, ehtest.exe, instead of the game. The program is
# compiled by the decompilation's toolchain/ VC6 and lifted by `cmake --build <dir> --target sfcop_ehtest_lift`
# (src/app/guest/tests/lift_ehtest.sh); once it has been, configuring again adds the test host and
# the CTest test sfcop_guest_tests. Without it, or without the lift, there is no guest test.
set(SFCOP_EHTEST_DIR "${CMAKE_BINARY_DIR}/ehtest")
add_custom_target(sfcop_ehtest_lift
    COMMAND sh "${CMAKE_SOURCE_DIR}/src/app/guest/tests/lift_ehtest.sh" "${SFCOP_EHTEST_DIR}"
            "${SFCOP_PCRECOMP_BUILD_DIR}"
    COMMENT "Compiling and lifting the guest test program"
    USES_TERMINAL
    VERBATIM)
if(EXISTS "${SFCOP_EHTEST_DIR}/out/gen/recomp_dispatch.c")
    set(_sfcop_ehtest_gen "${SFCOP_EHTEST_DIR}/out/gen")
    file(GLOB _sfcop_ehtest_c CONFIGURE_DEPENDS "${_sfcop_ehtest_gen}/*.c")
    set(_sfcop_ehtest_includes "${_sfcop_ehtest_gen}" "${_sfcop_win32hle}"
        "${SFCOP_PCRECOMP_BUILD_DIR}/runtime/recomp32")
    add_library(sfcop_ehtest_lifted STATIC ${_sfcop_ehtest_c})
    target_include_directories(sfcop_ehtest_lifted PRIVATE ${_sfcop_ehtest_includes})
    target_compile_definitions(sfcop_ehtest_lifted PRIVATE ${SFCOP_RECOMP_DEFINITIONS})
    target_compile_options(sfcop_ehtest_lifted PRIVATE -w -O2 -fno-strict-aliasing -fwrapv)
    add_executable(sfcop_guest_tests "${CMAKE_SOURCE_DIR}/src/app/guest/tests/GuestTestMain.cpp"
        ${SFCOP_GUEST_HOST_SOURCES})
    target_include_directories(sfcop_guest_tests PRIVATE "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/src/app/guest" ${_sfcop_ehtest_includes})
    target_compile_definitions(sfcop_guest_tests PRIVATE ${SFCOP_RECOMP_DEFINITIONS})
    target_compile_options(sfcop_guest_tests PRIVATE -fno-strict-aliasing -fwrapv)
    sfcop_strict_warnings(sfcop_guest_tests)
    target_link_libraries(sfcop_guest_tests PRIVATE sfcop_spike_unowned sfcop_src sfcop_win32hle
        sfcop_ehtest_lifted $<$<PLATFORM_ID:Windows>:SDL3::SDL3-static> SDL2::SDL2
        $<$<NOT:$<PLATFORM_ID:Windows>>:pthread> $<$<NOT:$<PLATFORM_ID:Windows>>:m>)
    if(TARGET sfcop_hle_guest)
        target_link_libraries(sfcop_guest_tests PRIVATE sfcop_hle_guest)
    endif()
    add_dependencies(sfcop_guest_tests SDL3-shared)
    add_test(NAME sfcop_guest_tests
        COMMAND sh "${CMAKE_SOURCE_DIR}/src/app/guest/tests/run_ehtest.sh"
                "$<TARGET_FILE:sfcop_guest_tests>" "${SFCOP_EHTEST_DIR}/root"
                "${CMAKE_BINARY_DIR}/ehtest-run")
else()
    message(STATUS "sfcop_ehtest: skip: the lifted test program is not built (no sfcop_guest_tests, "
                   "no CTest test sfcop_guest_tests); run `cmake --build ${CMAKE_BINARY_DIR} --target "
                   "sfcop_ehtest_lift`, then configure again")
endif()

# The machine's hand-over rule (cmake/lifter/patches/pcrecomp-quantum.patch): pcrecomp's
# host_lite.c from the clone and the test's own stubs, no lifted program. The CTest test
# sfcop_machine_quantum_tests.
add_library(sfcop_machine_quantum_host OBJECT "${_sfcop_win32hle}/host_lite.c")
target_include_directories(sfcop_machine_quantum_host PRIVATE "${_sfcop_win32hle}"
    "${SFCOP_PCRECOMP_BUILD_DIR}/runtime/recomp32")
target_compile_definitions(sfcop_machine_quantum_host PRIVATE ${SFCOP_RECOMP_DEFINITIONS})
target_compile_options(sfcop_machine_quantum_host PRIVATE -w -fno-strict-aliasing -fwrapv)
sfcop_win32hle_posix(sfcop_machine_quantum_host)
add_executable(sfcop_machine_quantum_tests
    "${CMAKE_SOURCE_DIR}/src/app/guest/tests/MachineQuantumTest.cpp")
target_include_directories(sfcop_machine_quantum_tests PRIVATE "${_sfcop_win32hle}"
    "${SFCOP_PCRECOMP_BUILD_DIR}/runtime/recomp32")
target_compile_definitions(sfcop_machine_quantum_tests PRIVATE ${SFCOP_RECOMP_DEFINITIONS})
target_link_libraries(sfcop_machine_quantum_tests PRIVATE sfcop_machine_quantum_host sfcop_doctest
    $<$<NOT:$<PLATFORM_ID:Windows>>:pthread>)
add_test(NAME sfcop_machine_quantum_tests COMMAND sfcop_machine_quantum_tests --no-version)
