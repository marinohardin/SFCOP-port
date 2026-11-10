# Cross toolchains

The `windows-x64` and `linux-x64` presets cross-compile the native client and its tests on the
macOS arm64 machine (the developer's Mac and the self-hosted runner `sfcop-mac`), with a pinned
LLVM and two pinned sysroots installed outside the repository. `macos-arm64` uses Apple clang and
needs none of this.

| File | What it is |
| --- | --- |
| `pins.env` | Every pin: version, URL, SHA-256 and size of each download; the tree hash of each installed sysroot; the container images by digest |
| `setup.sh` | Installs the pins under `SFCOP_TOOLS_DIR`; idempotent; refuses and removes a download or a tree whose hash is not the pin |
| `sysroot.py` | `tree-hash` (the content hash a sysroot is pinned by) and `relink` (absolute symlinks made relative) |
| `pins.cmake` | Read by both toolchain files: the pins, the tools folder, LLVM |
| `windows-x64.cmake`, `linux-x64.cmake` | The toolchain files |
| `windows-arm64.cmake`, `linux-arm64.cmake` | The arm64 toolchain files, for the release packages only (`packaging/README.md`); PR CI does not build them |
| `run-windows.sh`, `run-linux.sh` | `CMAKE_CROSSCOMPILING_EMULATOR`: run a built program under Wine, or in a `linux/amd64` container |
| `check-readable.sh` | CI's first step on a cross preset: fails in 20 seconds, naming the cause, if the toolchain cannot be read (see "The runner") |

## Use

```sh
sh cmake/toolchains/setup.sh                 # once per machine; or: setup.sh llvm linux windows
cmake --preset linux-x64 -B <build> -DSFCOP_LIFT=OFF
cmake --build <build> -j4 --target sfcop_tests
ctest --test-dir <build> --output-on-failure  # in the container
```

The same for `windows-x64` (tests under Wine). With `-DSFCOP_LIFT=ON`, `SFCOP_INSTALL_DIR` and
`SFCOP_PCRECOMP_DIR`, the build also lifts the original and builds `sfcop`. `ctest --preset <p>`
works on the default build folder `build/<p>`.

### The arm64 presets

`linux-arm64` and `windows-arm64` exist for the release packages. `sh cmake/toolchains/setup.sh
linux-arm64 windows-arm64` installs their sysroots (the default `setup.sh` with no argument does
not). Windows arm64 uses the same MSVC CRT and Windows SDK pins as x64, laid out by xwin for
`aarch64`. Linux arm64 cannot use the Steam Runtime 3, which has no arm64 build; its sysroot is
Debian 12 "bookworm" arm64 (glibc 2.36, GCC 12's libstdc++), made by the pinned `debian:bookworm-slim`
image from the Debian snapshot archive at the pinned time (`LINUX_ARM64_SYSROOT_*` in `pins.env`)
and pinned by its tree hash. `linux-arm64`'s tests run in that same image, natively on an arm64
Mac (`run-linux.sh` with `SFCOP_RUN_PLATFORM=linux/arm64`; `run-tests.sh linux-arm64`).
`windows-arm64` has no emulator: Wine on the Mac does not run Windows arm64 programs, so its tests
are built and not run here.

## Tests

`ctest` runs `sfcop_tests` through the preset's emulator: `run-linux.sh` starts it in the pinned
Debian 11 container (`--platform linux/amd64`, no network, the build folder and the checkout
mounted at their own paths), `run-windows.sh` under Wine in a prefix of its own in the build
folder. A test that needs a GPU skips with its reason in both (no device in the container; no
Vulkan under Wine).

CI runs `sh run-tests.sh <preset> <build folder>` instead, which leaves the test cases that
`expected-failures.tsv` names for the preset out of the verdict and runs them once more for the
record. Each line names its entry under "Known failures" below, and gives the defect in `src/` or
the Wine behaviour that makes the case fail; a case that passes again is reported. The list is
never a way to pass a new failure: a line is added only with its entry, in a pull request the
integrator reviews, and removed when the fix lands.

| File | What it is |
| --- | --- |
| `expected-failures.tsv` | preset, test case, entry, reason |
| `run-tests.sh` | The CI test run of a cross preset |

## Artifacts

Each CI build job keeps, for 7 days and visible only to the repository's owner, `sfcop_tests` (or
`sfcop_tests.exe`), the configure log and the test log. The lifted `sfcop` is not among them: it
is derived from the retail executable, so it never goes to GitHub's storage, even in a private
repository, and neither do SDL's libraries. The owner builds `sfcop` locally.

## Known failures

The test cases `expected-failures.tsv` names, one entry per defect. The causes are suspected from
the failing assertion and the code it exercises; the owner named is the package that wrote the
module, who confirms the cause when fixing it.

KF-1 to KF-3 are Linux's, not an architecture's: `linux-arm64` lists the same test cases as
`linux-x64`.

**KF-1: a second `connect()` on a connected socket succeeds on Linux.** After a non-blocking
connect has completed, the host Transport calls `connect()` again and Linux returns 0 where macOS
returns `EISCONN`; the Transport reports success where `kWrongState` is documented
(`TransportHostTest.cpp:233`), and the winsock module passes the 0 on instead of
`WSAEISCONN` (`LoopbackTest.cpp:156`). Suspected: `TransportHost.cpp` relies on the errno macOS
gives. Owner: B7 (Transport host, winsock).

**KF-2: `send()` on a socket that never connected gives `EPIPE` on Linux.** The test expects
`kNotConnected` (`TransportHostTest.cpp:437`); Linux reports `EPIPE`, which the Transport maps to
a different error, where macOS gives `ENOTCONN`. Suspected: the errno mapping in
`TransportHost.cpp` lacks the Linux case. Owner: B7.

**KF-3: a Latin-1 name in the wrong case does not find its file on Linux.**
`sfcop_kernel_get_file_attributes("C:\\Assets\\CAF\xC9.TXT")` returns "not found" for `café.txt`
on a case-sensitive file system (`KernelBridgeTest.cpp:68`). Suspected: the kernel module's
case-insensitive lookup folds ASCII only. Owner: B5 (kernel HLE).

**KF-4: the host file system returns backslash paths on Windows.** `absolutePath` and the install
and data folders `Startup` records use `\` where the tests expect `/` (`FilesTest.cpp:62-65`,
`StartupTest.cpp:106-121`). Suspected: `HostPlatform_windows.cpp` and `Files.cpp` return native
separators and the tests, written on macOS, compare against generic ones. Owner: the app
skeleton (C4), now `src/app/` under A1.

**KF-5: removing a shot folder fails on Windows.** `GuestShotsTest.cpp:82` throws "sharing
violation" from `remove_all`. Suspected: a shot file is still open when the test removes its
folder; Windows refuses where macOS and Linux allow. Owner: B6 (guest runtime).

**KF-6: non-ASCII names do not round-trip through the Windows host file API.** The Latin-1 and
UTF-16 forms of `café.txt` do not find the file, and `WIN32_FIND_DATA` does not carry the name
back (`KernelBridgeTest.cpp:68`, `:71`, `:89`, `:110`). Suspected: the bridge converts names to
UTF-8 for a path the Windows host passes to an ANSI API. Owner: B5.

**KF-7: a case-only rename keeps the old case under Wine.** After renaming `logs/b.gf` to
`logs/B.gf`, the listing still shows `b.gf` (`StoreHostTest.cpp:283`). Suspected: Wine's prefix
sits on a case-insensitive APFS volume, which keeps the first spelling; not seen on Windows,
where NTFS renames the case. Owner: B5 (Store host), who confirms on a Windows machine.

**KF-8: a stack size past the end of the range starts the thread under Wine.**
`ThreadsHostTest.cpp:76` expects `kOutOfResources`. Suspected: Wine does not refuse the size
Windows does; not seen on Windows. Owner: B6.

**KF-9: no relative-motion event under Wine.** `WindowSdlTest.cpp:333` expects one event from
SDL's dummy video driver in relative mode. Suspected: the dummy driver on the Windows build never
delivers it; not seen on Windows. Owner: B1 (Window backend).

An unrelated limit, not a failing case: the lifted `sfcop` does not build for `windows-x64` yet.
pcrecomp's win32hle runtime includes POSIX headers (`unistd.h`, `pthread.h`, `strings.h`) and
calls `clock_gettime` and `nanosleep`, and `src/app/guest/GuestExceptions.cpp` leaves
`g_raiseShim` and `g_liftedCode` undeclared on Windows. CI builds and tests `src/` only on that
preset until both are fixed. Owners: the lifter (pcrecomp, integrator) and B6.

## What is installed

| Part | Pin | Installed at (under `SFCOP_TOOLS_DIR`) |
| --- | --- | --- |
| LLVM 22.1.8, the official macOS arm64 release: clang, clang++, clang-cl, lld (ld.lld, lld-link), llvm-ar, llvm-lib, llvm-rc, llvm-nm, llvm-objcopy, llvm-objdump, llvm-readobj, and clang's own headers | the archive's SHA-256 | `llvm/22.1.8/` |
| The Linux sysroot: the Steam Runtime 3 "sniper" SDK sysroot 3.0.20261021.266025 (Debian 11 base, glibc 2.31, GCC 10's libstdc++), x86_64 headers and link libraries only | the archive's SHA-256 and the tree hash | `sysroots/linux-x64/steamrt3-sniper-<version>/` |
| The Windows sysroot: the MSVC CRT 14.44.17.14 and the Windows SDK 10.0.26100, x86_64 desktop, from the Visual Studio 17.14.44 installer manifest, laid out by xwin 0.10.0 run in the pinned Alpine image | the installer manifest's SHA-256 (xwin checks each package against it) and the tree hash | `sysroots/windows-x64/msvc-<crt>-sdk-<sdk>/` |
| The Windows arm64 sysroot: the same, aarch64 | the same, and its own tree hash | `sysroots/windows-arm64/msvc-<crt>-sdk-<sdk>/` |
| The Linux arm64 sysroot: Debian 12 arm64 headers and link libraries (glibc 2.36, GCC 12), about 210 MB | the image digest, the snapshot time and the tree hash | `sysroots/linux-arm64/debian12-<snapshot>/` |

Nothing comes from Homebrew. Downloads land in `SFCOP_TOOLCHAIN_DOWNLOADS` (default
`<tools>/.downloads`) and are deleted once their tree is installed. The Windows sysroot is laid
out without xwin's case-folding symlinks, because the tools volume is case-insensitive APFS, where
they would collide with the files they alias; on a case-sensitive volume, drop `--disable-symlinks`
in `setup.sh`.

## The Linux baseline

The target is the SteamOS AppImage, so the baseline is the Steam Runtime 3 ("sniper") that Steam
runs games in: glibc 2.31 and GCC 10. Valve publishes its SDK as a sysroot tarball with a
SHA-256 list; `setup.sh` keeps from it only `usr/include`, GCC 10's `x86_64-linux-gnu` support
files, the glibc and libstdc++ link libraries, and the link libraries of what SDL3 probes for
(X11 and xcb, EGL and GL, Vulkan, DRM and GBM, ALSA, PulseAudio, PipeWire, JACK, udev, D-Bus,
IBus's GLib, libdecor, FriBidi, libthai, libusb), about 160 MB, and makes its absolute symlinks
relative, so it is reproducible from the pin. SDL3 finds those headers and libraries at configure
time (through the sysroot's `pkg-config` files when the Mac has `pkg-config`) and loads the
libraries at run time, so the binary depends on none of them. Wayland is off: SDL3 generates its
Wayland protocol code with `wayland-scanner`, a host tool the Mac does not have; X11 (and
Xwayland under SteamOS's compositor) and KMSDRM are on. libstdc++ and libgcc are linked
statically; the binary needs glibc 2.31 or later. Tests run in Debian 11 (`LINUX_RUN_IMAGE`, the
same glibc), with no network.

## The Windows driver

The owner chose the `x86_64-windows-msvc` ABI with clang-cl and lld-link against the MSVC CRT and
the Windows SDK (mingw-w64 is not used). The toolchain drives clang through its GCC-style driver
with `--target=x86_64-pc-windows-msvc`; the code generation, the ABI, the CRT and the linker
(lld-link) are those of clang-cl. The reason: the build's flags are GCC-style
(`cmake/SfcopWarnings.cmake`'s `-Wall -Wextra -Werror`, the lift's `-fno-strict-aliasing -fwrapv`),
and clang-cl reads `-Wall` as `-Weverything`, which with `-Werror` fails every file. clang-cl is
installed; using it needs `cmake/SfcopWarnings.cmake` to give MSVC-style flags (`/W4 /WX`) when
`CMAKE_CXX_COMPILER_FRONTEND_VARIANT` is `MSVC`. The CRT is linked statically (`/MT`).

## Licence

The MSVC CRT and the Windows SDK are Microsoft's, under the Microsoft Visual Studio licence terms
(the Visual Studio 2022 build tools and the Windows SDK licence). The owner of this repository
accepted those terms for this project on 2026-11-04; `setup.sh` passes xwin's `--accept-license`
on that acceptance and on no other. The sysroot stays on the owner's machines: it is never
committed, uploaded or put in an artifact.

## The runner

The runner needs no installer step; CI never downloads a toolchain. Its environment names:

| Variable | Meaning |
| --- | --- |
| `SFCOP_TOOLS_DIR` | The tools folder `setup.sh` installed into; the scripts stop with an error when it is unset |
| `SFCOP_LLVM_DIR`, `SFCOP_LINUX_SYSROOT`, `SFCOP_WINDOWS_SYSROOT` | Optional: one part somewhere else |
| `SFCOP_WINE_BIN` | Optional: the folder holding `wine` and `wineserver` (default: the Wine under the tools folder) |
| `SFCOP_CROSS_WINEPREFIX` | Optional: the Wine prefix the Windows tests use (default: `wine-prefix` in the build folder) |

A job whose toolchain is missing fails at configure with the line naming what is missing and
`setup.sh`.

### One-time macOS permission: Removable Volumes

The tools folder is on the external drive. The first time a process the runner started reads an
external volume, macOS asks for permission for the runner's `node` (the process it holds
responsible for everything a job starts), as "Removable Volumes" under Privacy & Security, Files
& Folders. The question appears on the Mac's screen and the read blocks until it is answered, so
on an unattended Mac a job hangs inside its first cross compiler run.

That is what stopped the first two runs of PR 50: `build (windows-x64)` and `build (linux-x64)`
sat in the configure step for the whole 45-minute timeout, `cmake` stalled in its first `clang-22`
run, and the job's cleanup killed both as orphans. The same toolchain, started from a terminal
(which has the permission) or later by the runner, configured in 32 seconds. The system log shows
the permission entry for `<runner>/externals/node24/bin/node` created at 23:53 local time on 4
November, 22 minutes after the second hung job was cancelled, and every run after it passed. The permission was not observed blocking the read (no
prompt appears in the log), so this is the cause that fits the log, the timing and the fix, not
one seen directly.

The permission is keyed to the path of the runner's `node`: a runner update that installs a newer
`externals/node*` needs it again. `check-readable.sh` is the safeguard: the cross jobs run it
first, and it fails after 20 seconds, naming the permission, instead of hanging; the step
timeouts bound anything else that stalls. Granting it ahead of time: System Settings > Privacy &
Security > Files & Folders (Full Disk Access also covers it) > allow the runner's `node`.
