# linux-x64: a stub. The preset configures, so its cache and compile settings can be checked in
# CI, but nothing is built: there is no cross toolchain on the runner yet. Wave 2 (A5, the CI
# matrix) replaces this file with a real one; the target is the SteamOS AppImage.
#
# Missing, for a real cross build from macOS:
#   - clang with ld.lld (LLVM; Apple's linker cannot link ELF);
#   - an x86_64 glibc sysroot old enough for the AppImage's baseline;
#   - SDL3 and miniaudio built for that target.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(SFCOP_PLATFORM linux-x64)
set(SFCOP_TOOLCHAIN_STUB ON)
set(SFCOP_TOOLCHAIN_MISSING "clang with ld.lld and an x86_64 glibc sysroot (Wave 2 A5)")
