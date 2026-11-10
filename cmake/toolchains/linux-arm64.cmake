# linux-arm64: cross-compiles from macOS with the pinned LLVM (clang, ld.lld) against a Debian 12
# "bookworm" arm64 sysroot (glibc 2.36, GCC 12's libstdc++); the Steam Runtime 3 that linux-x64
# targets has no arm64 build. setup.sh installs it (`setup.sh linux-arm64`). Used for the release
# packages (packaging/README.md), not by PR CI. The C++ runtime is linked statically, so the
# binary needs only glibc 2.36 or later. Tests run in the sysroot's own image, natively on an
# arm64 Mac (run-linux.sh with SFCOP_RUN_PLATFORM=linux/arm64).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(SFCOP_PLATFORM linux-arm64)
include("${CMAKE_CURRENT_LIST_DIR}/pins.cmake")

if(DEFINED ENV{SFCOP_LINUX_ARM64_SYSROOT})
    set(CMAKE_SYSROOT "$ENV{SFCOP_LINUX_ARM64_SYSROOT}")
else()
    set(CMAKE_SYSROOT
        "${SFCOP_TOOLS_DIR}/sysroots/linux-arm64/debian12-${SFCOP_PIN_LINUX_ARM64_SYSROOT_SNAPSHOT}")
endif()
sfcop_require_tool("${CMAKE_SYSROOT}/usr/include/stdio.h" "the Linux arm64 sysroot")

set(CMAKE_C_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_LINKER_TYPE LLD)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc")

set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR}
    "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig")

# ctest runs the binaries in the sysroot's image, natively on an arm64 Mac.
set(CMAKE_CROSSCOMPILING_EMULATOR
    "${CMAKE_COMMAND}" -E env SFCOP_RUN_PLATFORM=linux/arm64
    "SFCOP_RUN_IMAGE=${SFCOP_PIN_LINUX_ARM64_RUN_IMAGE}"
    sh "${CMAKE_CURRENT_LIST_DIR}/run-linux.sh")
