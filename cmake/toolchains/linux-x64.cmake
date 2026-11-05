# linux-x64: cross-compiles from macOS with the pinned LLVM (clang, ld.lld) against the Steam
# Runtime 3 "sniper" sysroot (glibc 2.31, GCC 10's libstdc++), the SteamOS AppImage's baseline.
# setup.sh installs both; README.md says how. The C++ runtime is linked statically, so the binary
# needs only glibc 2.31 or later. Tests and the boot run in a Debian 11 container
# (run-linux.sh, LINUX_RUN_IMAGE in pins.env).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(SFCOP_PLATFORM linux-x64)
include("${CMAKE_CURRENT_LIST_DIR}/pins.cmake")

if(DEFINED ENV{SFCOP_LINUX_SYSROOT})
    set(CMAKE_SYSROOT "$ENV{SFCOP_LINUX_SYSROOT}")
else()
    set(CMAKE_SYSROOT
        "${SFCOP_TOOLS_DIR}/sysroots/linux-x64/steamrt3-sniper-${SFCOP_PIN_LINUX_SYSROOT_VERSION}")
endif()
sfcop_require_tool("${CMAKE_SYSROOT}/usr/include/stdio.h" "the Linux sysroot")

set(CMAKE_C_COMPILER_TARGET x86_64-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET x86_64-linux-gnu)
set(CMAKE_LINKER_TYPE LLD)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc")

# SDL3 finds the X11, Wayland and audio headers through the sysroot's pkg-config files when the
# Mac has a pkg-config, and through its include folder otherwise; the libraries themselves are
# loaded at run time.
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR}
    "${CMAKE_SYSROOT}/usr/lib/x86_64-linux-gnu/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig")

# ctest and the boot run the binaries in the pinned container.
set(CMAKE_CROSSCOMPILING_EMULATOR sh "${CMAKE_CURRENT_LIST_DIR}/run-linux.sh")
