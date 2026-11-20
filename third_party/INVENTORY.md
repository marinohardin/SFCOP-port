# Third-party inventory

Two lists: the libraries the native build links (pinned in `CMakeLists.txt` here), and the
third-party code found inside the original exe (P0-6).

## Build dependencies of the native build

Pinned by a tagged release and the SHA-256 of its archive in `third_party/CMakeLists.txt`, fetched
at configure time into the build directory, never vendored. Only the integrator adds, removes or
bumps one (CONVENTIONS §5). `DependencyPinsTest.cpp` checks that the linked SDL3, miniaudio,
doctest and Dear ImGui are these; sdl2-compat is not linked into the tests.

| Name | Version | Pin | Licence | Where used | Why |
| --- | --- | --- | --- | --- | --- |
| SDL3 | 3.4.18 (2026-10-02) | release asset `SDL3-3.4.18.tar.gz`, sha256 `9c75cf16...4334a3`; tag commit `829a65d7` | zlib | `src/platform/` backends (Wave 1); today only `sfcop_tests` links it | Window, GPU (SDL GPU), input and events on all three platforms |
| miniaudio | 0.11.25 (2026-03-03) | commit archive `9634bedb`, sha256 `1a3a79b8...76fa55` | public domain (Unlicense) or MIT-0 | the Audio backend (Wave 1); today only `sfcop_tests` | The audio engine under the Miles and DirectSound 3D HLE |
| doctest | 2.5.3 (2026-07-06) | release asset `doctest-v2.5.3.tar.gz`, sha256 `eb4a3972...d5d50f`; tag commit `2d0a9359` | MIT | `sfcop_tests` only | C++ unit tests through CTest (CONVENTIONS §6) |
| Dear ImGui | 1.93.0 (2026-10-14) | commit archive `c1aaac65`, sha256 `b3d8f998...d847114`; tag `v1.93.0` is that commit | MIT | `sfcop_src` (the launcher, `src/app/launcher/`), so `sfcop` and `sfcop_tests`; the core with the `imgui_impl_sdl3` and `imgui_impl_sdlrenderer3` backends only | The launcher's screens (import, check, change the game files) on an SDL renderer, before the game's window opens |
| sdl2-compat | 2.32.74 (TEMPORARY: leaves with Wave 1 B1/B3) | release asset `sdl2-compat-2.32.74.tar.gz`, sha256 `ec68abde...8924588` | zlib | `sfcop` only, with `SFCOP_LIFT`: the boot platform (`src/app/boot/`) and win32hle; a shared library that loads the pinned SDL3, built shared beside `sfcop` | The SDL2 API the copied boot platform is written against |

Not a dependency, and not in this directory, but the build uses it until Wave 1 replaces the
temporary boot platform (`src/app/boot/`):

| Name | Version | Licence | How the build gets it | Why |
| --- | --- | --- | --- | --- |
| pcrecomp | commit `ef3f0da6` with `cmake/lifter/patches/` | MIT | a clone outside the repository (`SFCOP_PCRECOMP_DIR`, made by `cmake/lifter/setup.sh`) | The lifter (its Python tools, run in a container) and its win32hle runtime, which the boot platform links |

## Third-party code in StarFleetOP.exe 2.5.5.2 (P0-6)

Every evidence column below was produced by a script in `scripts/`; see `README.md` for how to rerun them.
"Vendored" means the code is copied into this directory; "reference" means it was read in a clone under
`work/src/` and left out of the repo.

| Component | Evidence in exe | Version | Source found | Licence | Vendored? | Port strategy |
|---|---|---|---|---|---|---|
| **Crypto++** | **None in any shipped binary.** Zero `@CryptoPP@@` RTTI and zero `CryptoPP` strings in the 2.5.5.2 exe, the 2.5.0.0 exe, `ServerPlatform.exe` and all 90 `.scr` scripts (`evidence/rtti_namespaces.md`). The libmatch "hits" are look-alikes. It exists only in the SDK as `cryptlibR.lib` plus 3 headers. | **3.1** (4/27/1999). Lib built from `D:\Work\Sfc\crypto31\`; its 90 objects equal 3.1's `cryptlib.dsp`; 3.1 features present, 3.2+ absent; the SDK headers equal 3.1 once Taldren's trace macros are stripped. | `cryptopp.com/crypto31.zip` (Wayback copy), sha256 `0d4eb329...4f5b` | Wei Dai compilation licence: mostly public domain, notice must be kept, patent caveat | **Yes**: `cryptopp/src/` (library sources and `.dsp`, no tests or `.dat` files) | Nothing to port. Kept for rebuilding the SDK lib. |
| **WONAPI** (WON.net client, generation 2) | `WONAPI::` RTTI classes (`AsyncOp`, `RefCount`, `CompletionBase`, `DirEntity`, `Auth2Certificate`, `GetDirOp`, `DetectFirewallOp`, `GetMOTDOp`, `BigInteger`, ...); 302 `WS_*` status strings; `SOFTWARE\WON\CDKeys` | 2001 | None usable | Proprietary, no licence | No | Clean-room reimplementation of the few ops the login uses, or replace the WON login with our own auth/lobby. |
| **GameSpy SDK**: GP, CD key, Chat (IRC), GHTTP, pt (vercheck), server list v1 (CEngine), QR v1 | Strings for every component (`gamespy/components.md`): `gpiInitialize`, `gpcm/gpsp/key/master/motd.gamespy.com`, `vercheck.asp`, `\heartbeat\%d\gamename\%s`, IRC `UTM`. No qr2 or SB2. Game name `strfltcmd2`. | 2000-2001 | None usable | Proprietary, no dependable licence | No | Stub GP, CD key and pt. Replace server list and QR with our own lobby (OpenSpy works for old clients). IRC chat is optional. |
| **MSVC 6 CRT** (LIBCMT, static `/MT`) | Rich header: CRT objects from builds 8797/8168/8447/8966; static CRT | VC6 SP4 (compiler 12.00.8799) | decomp.me `msvc6.4.tar.gz` at `work/toolchain/dl/` (sha256 `6d4ef930...3c57`, from `OmniBlade/decomp.me` release `msvcwin9x`) has `Bin/`, `Include/` (997 files), `MFC/` and `ATL/`. **It has no `Lib/` (no LIBCMT/LIBCPMT) and no CRT source.** | Microsoft EULA, not redistributable | **No.** Point at the tarball path and never copy headers into the repo | Port uses the host C runtime. For matching or identification, CRT functions need VC6 LIBCMT/LIBCPMT signatures from some other source (gap, reported to P0-2/P0-3). |
| **Dinkumware STL** (VC6 `<xtree>`, `<xstring>`, ...) | 113 `std::` RTTI names; template instantiations across the exe | VC6 headers plus Taldren's override set | Stock VC6 headers in the tarball's `Include/`. The SDK's `OP API - R2.1/STL/Include/` replaces 13 headers (ALGORITHM, DEQUE, FSTREAM, ISTREAM, LIST, MAP, MEMORY, SET, SSTREAM, STRING, XMEMORY, XSTRING, XTREE). | Dinkumware notice: "for use only in conjunction with a valid license for Microsoft Visual C++" | **No.** Reference `reference/.../STL/Include` in place | Compile matching code with include order SDK `STL/Include` first, then tarball `Include/`. The port uses the modern C++ standard library. |
| **Miles Sound System** | `mss32.dll`: 26 `AIL_*` imports (sample playback only: `AIL_startup`, `AIL_allocate_sample_handle`, `AIL_load_sample_buffer`, `AIL_set_sample_*`, `AIL_waveOutOpen`, `AIL_set_DirectSound_HWND`, `AIL_serve`, ...); `Mss32.dll` ships with 343 exports | Miles **6.1a** (shipped `Mss32.dll` version resource, "Copyright 1991-2001 RAD Game Tools") | `mss.h` is widely mirrored, but it is RAD's proprietary SDK | Proprietary, not redistributable | **No** | Write a tiny `AIL_*` shim (26 functions, sample playback only) over miniaudio/SDL3 audio behind QSI Tiger's `tSndMgr`. The prototypes can be recovered from the exe's call sites. |
| **MicroQuill SmartHeap** | **Statically linked**: strings `SmartHeap Library`, `SmartHeap602MutexMovShrName`, `SmartHeap error handler recursively re-entered.` The exe does **not** import `SHW32.DLL` or `HA312W32.DLL` (`evidence/middleware_imports.md`). The DLLs ship in the install folder anyway; `HA312W32` is HeapAgent 3.12, the debug heap. | SmartHeap 6.02, from the mutex name (the shipped `SHW32.DLL` is 6.00.3 and `HA312W32.DLL` is HeapAgent 3.12.3) | None public | Proprietary | **No** | Identify its functions (`MemAlloc*`, `shi_*`, operator new/delete overrides) as library code and map them to system `malloc`/`new`. |
| **DbgHelp** | 9 imports (`SymInitialize`, `StackWalk`, `SymGetSymFromAddr`, `SymGetLineFromAddr`, ...): the game's own crash call-stack dump | Shipped `dbghelp.dll` 5.00.2195.1 (Windows 2000) | Windows SDK | Microsoft redistributable | No | Drop, or replace with platform crash reporting (Crashpad, or signal handler + backtrace). |
| **BugslayerUtil** (John Robbins, *Debugging Applications*, 2000) | **Not imported by the game, the server or the scripts.** The DLL ships (35 exports: `SetCrashHandlerFilter`, `GetFaultReason`, `DiagAssert*`, ...) and links the *debug* CRT `MSVCRTD.dll`. The SDK also has `BugslayerUtil.lib`, and the script templates link it. | `BugslayerUtil.dll` 3.0.000, "Copyright 1997-2000 by John Robbins" | Book companion source (copyright John Robbins / Microsoft Press; free to use in your own programs per the book, **not verified**) | Book licence, unverified | No | Not needed: no code of it is in the exe. Ignore. |
| QSI dmp3d / Tiger, Taldren Common/Switch | First-party engine code | | SDK libs and headers (outside the repository) | Proprietary game code | n/a (the decomp target, not third-party) | Decompile; see P0-3/P0-7 |

### Notes for other tasks

- **P0-3 / P0-7 (symbol matching).** Treat every `cryptlibR.lib` match in `analysis/results/*_sdklib_matches.tsv`
  as a false positive. Crypto++ is not in the exe. The 32 addresses involved are relocation-masked STL template
  bodies and tiny accessors.
- **P0-2 (toolchain).** `msvc6.4.tar.gz` has no `Lib/` directory. Linking an exe or `.scr`, or matching CRT code,
  needs `LIBCMT.LIB`, `LIBCPMT.LIB`, `OLDNAMES.LIB` and the Win32 import libs from another VC6 SP4 source.
- **Netplay / Dynaverse.** RTTI shows WONAPI callbacks only in `tWonLogon` and `tWONInterSwitchConnection`,
  which belong to the Dynaverse II login and the inter-switch link. GameSpy appears tied to Arcade, the server
  list, presence and patch checks. Neither claim has been checked against cross-references yet. Confirm in
  Ghidra (P0-3) before relying on "skirmish and direct-IP need neither".
