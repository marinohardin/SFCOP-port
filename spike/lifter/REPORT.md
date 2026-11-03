# P0-4 lifter spike (Route L): report

Date: 2026-11-02. Target: `StarFleetOP.exe` 2.5.5.2 (sha256 `264da726…c883`, verified).
Toolkit: `sp00nznet/pcrecomp` at `ef3f0da` (2026-10-30), driven the way `redalert2-recomp` drives it.
All numbers below come from scripts in `scripts/`; raw outputs are in `results/`, and the lifted tree
and build are in `work/lifter/`. How to rerun everything: `README.md`.

## Verdict

**Conditional go for Route L.** The whole exe lifts to C with 0 lifter errors. The C compiles and
links natively for arm64 macOS with Apple clang in 6 minutes. The binary maps the exe and runs
the static CRT, the C++ static constructors and **WinMain, which registers its class and creates
its window in 0.61 s** (`results/run3.err.txt`, `results/run3_imports_after_winmain.txt`). It then
stops at the first import win32hle lacks, `DirectInputCreateA`.

Two cheap host fixes were needed to get that far (static TLS, and the OSSpecific DLL). The
condition on the "go" is guest C++ exception handling. It is certain to be needed, it has no
runtime on macOS today, and it is the one blocker that touches the lifter as well as the runtime.

A major finding changes the renderer plan:

- **The game is DirectX 6, not 7.** It references IID_IDirectDraw4, IID_IDirectDrawSurface4 and
  IID_IDirect3D3 and none of the DX7 IIDs, and it imports `DirectDrawCreate`, not `DirectDrawCreateEx`.
- **win32hle already covers it.** That is the interface set win32hle's `ddraw.c`/`d3d.c` implement
  (software rasteriser). Every Device3/DDraw4/Surface4/D3D3 method slot found statically has an
  implementation there; only `Surface::IsLost`/`Restore` are success stubs.

`PROPOSAL.md` 2.3 and the P1-1 title ("D3D7/DDraw HLE") should be corrected.

Recommendation versus the delink route:

- **Route L** is the fastest way to a native Mac build. My estimate is 4 to 6 weeks to the main
  menu (blockers 1, 2 and 4 below).
- **Delink (P1-2)** remains the right base for byte-matching decompilation.
- **Together:** the two share the function catalogue. Lifted functions can be replaced by
  decompiled C one at a time, because every function is its own `sub_XXXXXXXX` with the original
  calling convention.
- **Next gate:** proceed with Route L as the platform track, and re-decide at "main menu renders
  under win32hle with guest EH working".

## Measurements

| What | Result | Source |
|---|---|---|
| RTTI recovery | 2,033 classes, 2,149 vtables, 6,728 virtual methods (1 s) | `results/rtti.log` |
| Function discovery (disasm32, RTTI-seeded) | 55,622 entries: **27,623 starts + 27,999 aliases** (22,036 of them C++ EH stubs/funclets, 1,824 labels inside bodies); 3.49M instructions; 97.9% of `.text` bytes covered; 219 s | `results/disasm.log` |
| Of which only via data pointers | 19,214 (unaligned scan of data for code pointers); 7,117 pointer targets rejected as mid-instruction | same |
| Lift (lift32 via `run_lift.py --all`) | 55,623 bodies, **0 lifter exceptions**, 0 not-lifted stubs, 304 bodies without a terminator; 1.80M instructions in bodies; **4.18M lines / 191 MB of C in 140 files**; 84 s (109 s wall in the container) | `results/lift.log`, `results/lift_stats.json` |
| Unimplemented instructions (lifted as a silent `;`) | 339 sites, 15 mnemonics. **3DNow!** `pfmul` 108, `pfadd` 43, `pfacc` 38, `femms` 21, `pfrsqrt` 4, `pfrcp` 3, `pfrcpit1/2`, `pfrsqit1` (about 20 functions). **Data decoded as code** `lcall` 94, `int` 16, `arpl` 3, `sti` 1 (94 functions). **x87 environment** `fnstenv` 2, `fldenv` 2 | `results/lift_stats.json` |
| Indirect calls in the lift | 32,518 `call [reg+off]` (virtual dispatch and function-pointer fields), 927 `call reg`, 3,627 `call [IAT]`, 108 `call [global]`. All go through `RECOMP_ICALL`: a binary search of the 55,623-entry dispatch table, then the import shims. An unknown target logs once and returns `eax=0` | `results/lift_stats.json`; grep of `out/gen` |
| C++ EH, static | 5,523 VC6 FuncInfo tables (all `__CxxFrameHandler` at 0x00735F0C, plus 1 at 0x005B802C); 16,979 unwind states; **127 functions with try blocks** (148 try, 167 catch handlers); 24 SEH frames (`_except_handler3` 0x0073B018, CRT). **`_CxxThrowException` = 0x00737479, called from 1,174 sites** | `results/eh_census.json` |
| C++ EH, in the lift | `eh32.resume_points` recognised 92 functions' frames (86 with catch continuations). Only **32 functions got a landing pad** (`RECOMP_EH_ENTER`), because a continuation must lie inside the lifted body | `results/lift_stats.json`; grep |
| `fs:` use | 1,122 `mov eax, fs:[0]`, 1,119 `mov fs:[0], esp` (frame installs), 5,942 `mov fs:[0], ecx` (restores); **3 reads of `fs:[0x2C]` (static TLS)** | `results/eh_census.json` |
| Imports vs win32hle | 353 imports; 278 have a shim of the same name; **75 missing**: MSS32 21 of 26, DBGHELP 9, KERNEL32 22 (16 are `*W` wide-char file APIs), COMDLG32 5, USER32 6, WSOCK32 5 (`accept`, `select`, `__WSAFDIsSet`, `getpeername`, `shutdown`), ADVAPI32 3, GDI32 2, DINPUT 1, DDRAW 1 (`DirectDrawCreateClipper`). "Covered" includes stubs: `RaiseException` exits the process and `RtlUnwind` is a no-op | `results/imports_vs_win32hle.tsv` |
| arm64 build | Apple clang 21, `-O2`, 4 jobs: **350 s** full build, 0 errors; Mach-O arm64 **105 MB** (96 MB `__text`); guest 4 GB at fixed host 0x100000000000 | `results/build.log` |
| Boot (headless, 60 s watchdog) | Run 1: CRT plus about 96k critical-section calls into the static constructors, then died on a guest `throw` (OSSpecific DLL not loadable). Run 2: guest NULL dereference via `fs:[0x2C]` in a thread start. **Run 3: reaches WinMain at 0.61 s, then RegisterClassA, CreateWindowExA, SetWindowPos, CreateThread, a directory scan, then FATAL `DirectInputCreateA`.** One static constructor (0x0059644A) was skipped silently: `ICALL unresolved` from `_initterm` | `results/run{1,2,3}.err.txt` |

DirectX method inventory (static lower bound; full table in `results/dx_inventory.md`):

| Interface | Slots the game calls | win32hle |
|---|---|---|
| IDirect3D3 | 2 Release, 6 CreateViewport (+ unattributed 3/8, probably EnumDevices/CreateDevice) | implemented |
| IDirect3DDevice3 | 3 GetCaps, 8 EnumTextureFormats, 9 BeginScene, 10 EndScene, 22 SetRenderState, 28 DrawPrimitive, 38 SetTexture, 40 SetTextureStageState, 41 ValidateDevice | all implemented (software) |
| IDirectDraw4 | 0, 2, 4 CreateClipper, 5 CreatePalette, 6 CreateSurface, 10 FlipToGDISurface, 12 GetDisplayMode, 19 RestoreDisplayMode, 20 SetCooperativeLevel, 21 SetDisplayMode | implemented |
| IDirectDrawSurface4 | 2, 5 Blt, 20 GetPalette, 24 IsLost, 25 Lock, 27 Restore, 32 Unlock | implemented; IsLost/Restore are stubs |
| IDirectSoundBuffer | 2, 4, 9, 11, 12, 13, 15, 16, 18, 19, 20 | implemented |
| IDirectSound3DBuffer / 3DListener | SetPosition, SetVelocity, SetConeOrientation, SetConeOutsideVolume; SetDistanceFactor, SetOrientation, SetPosition, SetVelocity | **absent** |
| IDirectInput / IDirectInputDevice (keyboard; GUID_SysKeyboard) | CreateDevice; Acquire, Unacquire, SetDataFormat, SetCooperativeLevel (GetDeviceState not seen statically) | **absent** |
| DirectPlay 4 (CLSID_DirectPlay, IID_IDirectPlay4A, Lobby3A via CoCreateInstance) | code at 0x00616700 to 0x00619DC0; 15 catch handlers for `tDirectPlayError` | **absent**. `PROPOSAL.md` says "no DirectPlay": the import table doesn't show it, the COM path does |

The method attribution is per object field, made by slot pattern inside the code ranges that
reference each IID (`scripts/dx_inventory.py` documents the rule). About 120 call sites in dmp3d
stay unattributed. The dmp3d engine is linked statically; its `dmp3d_*.dll` plugin probe finds
no DLLs in the install.

## How pcrecomp handles the hard parts (summary; detail and citations in `MECHANISMS.md`)

- **Memory model.** A 32-bit guest runs in a 4 GB block (`RECOMP_MEM_BASE`, 0x100000000000 on
  macOS); a guest pointer is a `uint32`. Registers are globals (`RECOMP_LOCAL_REGS` caches them
  per function). One machine lock serialises all guest threads.
- **`__thiscall` and virtual dispatch.** These need nothing special: `ecx` is a register like any
  other, and `call [eax+N]` becomes a runtime lookup of the target VA in the sorted dispatch table.
  Every RTTI vtable slot is a seed, so vtable targets are lifted. Here all 6,728 were already in
  the catalogue.
- **RTTI.** Used for discovery and naming only. `dynamic_cast`/`typeid` run as lifted CRT code
  over the guest's own RTTI data.
- **C++ exceptions.**
  - **What exists:** frames lift as plain stores to `fs:[0]`. pcrecomp's 32-bit EH runtime
    (`runtime/native32/eh32.c`) walks the guest chain, runs unwind and catch funclets as guest
    code and longjmps to a landing pad.
  - **The gap:** it is Windows-only (native32), keyed to an imported `_CxxThrowException`, and
    requires an `ebp` frame. In win32hle `RaiseException` ends the process.
- **x87.** Modelled as `double[8]`: no 80-bit precision. The rounding mode is honoured
  (`fistp`/`_ftol`); precision control is ignored. NFSU2Recomp, the arm64 Metal fork, does the
  same with NaN fixes.

## Blockers, ranked

1. **Guest C++ exceptions: high severity, about 2 to 3 weeks.** This is certain to be hit on
   normal paths.
   - **Why it's certain:** the catch types include `tApplication_QuitException`,
     `tGameStateMgr_QuitException` and `tAICalculation::tThreadFinishedException` (control flow,
     not errors), WONAPI `ReadBufferException`, `tDirectPlayError` and `std::exception`. There are
     1,174 throw sites.
   - **Work:**
     - Port `eh32.c` into a win32hle `RaiseException(0xE06D7363)` handler. The CRT is lifted, so
       intercept there, or hook 0x00737479 by address.
     - Replace `__declspec(thread)`/`IsBadReadPtr`.
     - Make `RtlUnwind` real.
     - Extend landing-pad coverage from 32 to all 127 catching functions (prologue variants, and
       continuations outside the body).
     - Test against the census.
2. **Missing middleware HLE: high severity (it is the current stop), about 4 to 6 weeks in total.**
   - **DirectInput keyboard device:** 2 to 3 days.
   - **Miles sample API (21 `AIL_*` calls under QSI Tiger):** 1 to 2 weeks for real audio on
     SDL/miniaudio. Answering it silently gets past boot in a day.
   - **DirectSound3D buffer and listener:** about 1 week.
   - **WinSock `select`/`accept`/`shutdown`/`getpeername`/`__WSAFDIsSet`:** 2 to 3 days. Needed
     for any multiplayer.
   - **Smaller gaps, about 1 week:** the wide-char file APIs,
     `ChangeDisplaySettingsA`/`EnumDisplaySettingsA`, `DirectDrawCreateClipper`, and stubs for
     DBGHELP/COMDLG32/event log.
   - **DirectPlay 4:** only if a menu path uses it.
3. **Renderer quality and performance: medium severity, 4 to 8 weeks for a GPU backend.** It is
   not a boot blocker because the DX6 surface is covered in software. Unknowns:
   - how faithful win32hle's rasteriser is to dmp3d's states (29 `SetTextureStageState` sites);
   - its speed at the game's resolutions on M1.

   Plan a Metal (SDL GPU) implementation of the same DX6 interfaces.
4. **Function catalogue precision and silent ICALL misses: medium severity, about 1 week.**
   - **Size:** 27,623 starts is at the top of the 15k to 25k estimate. 19,214 entries came only
     from an unaligned data-pointer scan, and the catalogue is unscored until P0-3's Ghidra
     function list exists (`disasm/score_recovery.py` takes it).
   - **Known miss:** a static constructor reachable only through the `_initterm` table
     (0x0059644A) was missed. Because the runtime answers an unknown target with `eax=0`, it was
     skipped silently.
   - **Fix:** seed from the CRT initialiser tables and Ghidra/FID, turn on a strict ICALL abort,
     and treat "data decoded as code" (the 94 `lcall` functions) as false positives.
5. **x87 precision: medium for netplay and the oracle, low for boot; 1 to 2 weeks if needed.**
   - **The divergence:** on Windows, a DX6 device switches the FPU to 24-bit precision unless the
     game asks for `DDSCL_FPUPRESERVE`. The lifted build computes in 53-bit doubles.
   - **Who it affects:** a native client may diverge from Windows clients in any lockstep or
     replay comparison.
   - **Next step:** measure the control word the game runs with (oracle, P0-5) before deciding on
     PC-aware rounding.
6. **3DNow! paths: low severity, about 1 day.** About 20 functions. Make the lifted `cpuid`
   report no 3DNow! (or check what it reports now) so those paths are never taken.
7. **Loader and TIB gaps: low severity, done in the spike.**
   - **Static TLS:** `fs:[0x2C]` needed a 2-line pcrecomp patch (`patches/pcrecomp-tib-hook.patch`)
     plus 30 lines in the host.
   - **OSSpecific DLL:** win32hle's `LoadLibraryA` neither appends `.dll` nor runs DLL code.
     OSSpecific95/98.dll's single export (cdecl `InterlockedCompareExchange`) is now answered by
     the host.

Not blockers: `__thiscall` and virtual dispatch (they work), RTTI (data only), the build loop
(6 minutes, 105 MB binary), and SmartHeap (`SHW32`/`HA312W32` ship but the 2.5.5.2 exe does not
import them; another `PROPOSAL.md` 2.3 correction).

## Notes and limits

- **Third-party code.** pcrecomp's Python ran only inside a Docker container with no network
  (`scripts/sandbox.sh`); the auto-mode classifier refused running it on the host. win32hle's C
  is compiled into the host binary, which runs natively.
- **Docker disk use.** The container image (`sfcop-lifter-py`, python:3.13-slim + capstone,
  pefile, unicorn) lives in Docker Desktop's VM on the internal disk, about 200 MB.
- **SDL2_ttf.** It is not installed, so `host/include/SDL2/SDL_ttf.h` stubs it (GDI text draws
  nothing). SDL2 is Homebrew's `sdl2-compat`.
- **Static lower bound.** The DX inventory is static. Calls made through other code shapes (for
  example the keyboard poll) are missing, so expect more slots at run time.
- **Not done.** Not tried: a non-headless run, the 2.5.0.0 exe, `difftest.py` semantic
  verification, the alternative lifters (NFSU2's, M-HT/SR). They were not needed to make the
  blockers clear.
