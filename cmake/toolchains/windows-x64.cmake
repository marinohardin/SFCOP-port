# windows-x64: a stub. The preset configures, so its cache and compile settings can be checked
# in CI, but nothing is built: there is no cross toolchain on the runner yet. Wave 2 (A5, the CI
# matrix) replaces this file with a real one.
#
# Missing, for a real cross build from macOS:
#   - a clang-cl and lld-link (LLVM; Apple clang has neither);
#   - the Windows SDK and MSVC CRT headers and import libraries, as a sysroot;
#   - SDL3 and miniaudio built for that target.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(SFCOP_PLATFORM windows-x64)
set(SFCOP_TOOLCHAIN_STUB ON)
set(SFCOP_TOOLCHAIN_MISSING
    "clang-cl and lld-link, a Windows SDK and MSVC CRT sysroot (Wave 2 A5)")
