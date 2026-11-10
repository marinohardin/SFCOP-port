# windows-arm64: cross-compiles from macOS with the pinned LLVM for the aarch64-windows-msvc ABI,
# exactly as windows-x64.cmake does for x86_64 (it says why clang's GCC-style driver is used and
# not clang-cl), linked by lld-link against the aarch64 MSVC CRT and Windows SDK libraries
# (`setup.sh windows-arm64`). The CRT is linked statically. Used for the release packages
# (packaging/README.md), not by PR CI. No emulator: Wine on the Mac does not run Windows arm64
# programs, so the tests of this preset are built but not run here.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR ARM64)
set(SFCOP_PLATFORM windows-arm64)
include("${CMAKE_CURRENT_LIST_DIR}/pins.cmake")

if(DEFINED ENV{SFCOP_WINDOWS_ARM64_SYSROOT})
    set(SFCOP_WINDOWS_SYSROOT "$ENV{SFCOP_WINDOWS_ARM64_SYSROOT}")
else()
    set(SFCOP_WINDOWS_SYSROOT "${SFCOP_TOOLS_DIR}/sysroots/windows-arm64/msvc-${SFCOP_PIN_WINDOWS_CRT_VERSION}-sdk-${SFCOP_PIN_WINDOWS_SDK_VERSION}")
endif()
sfcop_require_tool("${SFCOP_WINDOWS_SYSROOT}/crt/include/vcruntime.h" "the Windows arm64 sysroot")
sfcop_require_tool("${SFCOP_LLVM_DIR}/bin/lld-link" "lld-link")

set(CMAKE_C_COMPILER_TARGET aarch64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET aarch64-pc-windows-msvc)
set(CMAKE_RC_COMPILER "${SFCOP_LLVM_DIR}/bin/llvm-rc")
set(CMAKE_LINKER "${SFCOP_LLVM_DIR}/bin/lld-link" CACHE FILEPATH "Linker")
set(CMAKE_LINKER_TYPE LLD)
# The static release CRT in every configuration: the sysroot carries no debug CRT.
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)

set(_sfcop_win_includes
    "${SFCOP_WINDOWS_SYSROOT}/crt/include"
    "${SFCOP_WINDOWS_SYSROOT}/sdk/include/ucrt"
    "${SFCOP_WINDOWS_SYSROOT}/sdk/include/um"
    "${SFCOP_WINDOWS_SYSROOT}/sdk/include/shared"
    "${SFCOP_WINDOWS_SYSROOT}/sdk/include/winrt")
# The MSVC CRT deprecates standard C (getenv, fopen, strcpy) and POSIX names in favour of its own
# _s and _-prefixed variants; src/ is portable C++ and keeps the standard names, so those two
# deprecation families are off (with -Werror they would fail every file that calls getenv).
set(_sfcop_win_flags " -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_WARNINGS")
# After clang's own headers, as clang-cl orders them: the CRT's intrin.h and xmmintrin.h only
# declare the intrinsics that clang's define.
foreach(_sfcop_dir IN LISTS _sfcop_win_includes)
    string(APPEND _sfcop_win_flags " -idirafter \"${_sfcop_dir}\"")
endforeach()
set(CMAKE_C_FLAGS_INIT "${_sfcop_win_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_sfcop_win_flags}")
# -I, not /I: with clang's GCC-style driver CMake preprocesses resource scripts with clang.
set(CMAKE_RC_FLAGS_INIT
    "-I \"${SFCOP_WINDOWS_SYSROOT}/sdk/include/um\" -I \"${SFCOP_WINDOWS_SYSROOT}/sdk/include/shared\"")
set(_sfcop_win_libs
    " -L\"${SFCOP_WINDOWS_SYSROOT}/crt/lib/aarch64\""
    " -L\"${SFCOP_WINDOWS_SYSROOT}/sdk/lib/um/aarch64\""
    " -L\"${SFCOP_WINDOWS_SYSROOT}/sdk/lib/ucrt/aarch64\"")
string(JOIN "" _sfcop_win_libs ${_sfcop_win_libs})
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_sfcop_win_libs}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_sfcop_win_libs}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_sfcop_win_libs}")

# No CMAKE_CROSSCOMPILING_EMULATOR: nothing on the Mac runs a Windows arm64 program.
