# spike/lifter/hle: P1-2, filling win32hle's gaps for StarFleetOP.exe

The P0-4 spike (`../README.md`) lifted the whole exe and stopped at the first import win32hle
lacks. This package adds the HLE the game needs on top of pcrecomp's win32hle, as our own
modules plus three small pcrecomp patches. With it the lifted game boots natively on arm64 macOS
to the CD-key prompt. Findings are in `REPORT.md`.

## Layout

| Path | What | Upstreamable |
|---|---|---|
| `src/dinput.c` | DirectInput 7 keyboard: `DirectInputCreateA`, IDirectInputA, IDirectInputDevice(2/7)A on SDL2's keyboard state; immediate and buffered data | yes |
| `src/mss_sample.c` | Miles AIL_* digital-sample API: waveOut driver, sample handles, double-buffered PCM/IMA ADPCM playback on an SDL2 audio device; `--silent` keeps the timing and outputs nothing. Also overrides `AIL_set_redist_directory` (win32hle's returns a host pointer) | yes |
| `src/ds3d.c` | IDirectSound3DBuffer and IDirectSound3DListener from `IDirectSoundBuffer::QueryInterface`: parameters stored and returned, not yet applied (table below) | yes |
| `src/kernel32_wide.c` | The 15 KERNEL32 `...W` calls the game imports, on top of win32hle's `...A` calls | yes |
| `src/misc_stubs.c` | `DirectDrawCreateClipper`, and the KERNEL32/USER32/GDI32/COMDLG32/ADVAPI32/DBGHELP calls win32hle lacks (list below) | yes |
| `src/hle_util.c`, `src/sfcop_hle.h` | Shared helpers: hand a call on to win32hle's own shim, GUID compare, UTF-16/UTF-8, one-shot TODO log | yes |
| `src/sfcop_game.c` | Game-only: the host answers for the OSSpecific DLL, the static constructor the catalogue missed, the guest throw logger | no |
| `host/` | The host (`main.c`, from the spike's, with the options below) and `CMakeLists.txt` | no |
| `tests/hle_selftest.c` | Calls every module's shims as the guest would and checks results and stack purge; Winsock over loopback | with the modules |
| `patches/pcrecomp-wsock32-select-accept.patch` | `select`, `accept`, `shutdown`, `getpeername`, `__WSAFDIsSet` in win32hle's wsock32.c | yes |
| `patches/pcrecomp-kernel32-unnamed-objects.patch` | Events, mutexes and semaphores created with name `""` are unnamed, not one shared object | yes (bug fix) |
| `patches/pcrecomp-manual-lookup-hook.patch` | `hle_manual_hook`: lets a host answer `recomp_lookup_manual` (applies on top of `../patches/pcrecomp-tib-hook.patch`) | yes |
| `scripts/run_all.sh` | Reruns everything: setup, build, self-test, runs |
| `results/` | Every measurement `REPORT.md` quotes, and one downscaled screenshot of the CD-key prompt (`cdkey_prompt.png`) |

The modules register their shims before `recomp_host_init()`; win32hle resolves a name to its
first registration, so ours win where both answer (COM method names included). An override that
only adds to win32hle's behaviour hands the call on with `sfcop_hle_original(name)`.

Generated data lives outside the repo in `$SFCOP_ROOT/work/lifter-hle/`:
APFS clones of the spike's `pcrecomp/` (with the patches applied), `game/`, `gameroot/`, `cat/` and
`out/gen/`, plus our `build/`, `runs/` and `shots/`.

## Rerun

Prerequisites: the spike's output in `work/lifter` (its `run_all.sh`), Xcode command line tools,
Homebrew `cmake`, `ninja`, `pkg-config` and `sdl2`. No Docker: nothing here runs pcrecomp's Python.

```bash
$SFCOP_ROOT/SFC2-decomp/spike/lifter/hle/scripts/run_all.sh
```

The default steps are `setup build selftest run plain`:

- `setup` clones the spike's tree and applies the four patches (it says when one is already applied).
- `build` takes about 5 minutes at `-j4` the first time, for the lifted C. After that, editing a
  module or the host relinks in seconds. `results/build.txt` is from such an incremental relink
  (1.1 s), not from a first build.
- `selftest` runs `build/hle_selftest` with SDL's dummy drivers.
- `run` is the headless, traced boot to a 60 s watchdog.
- `plain` is the same boot untraced.

`window` is not in the default steps because it opens a window on this Mac. It runs silent and
reports the frame rate:

```bash
STEPS="window" $SFCOP_ROOT/SFC2-decomp/spike/lifter/hle/scripts/run_all.sh
```

Host options added in this package:

| Option | Effect |
|---|---|
| `--silent` | Miles samples make no sound; handles, status and buffer timing still behave. Headless runs are always silent |
| `--shots DIR` | Write screenshots (24-bit BMP) into DIR: a final one at the watchdog, and periodic ones with `--shot-every` |
| `--shot-every S` | One screenshot every S seconds. Windowed, it is the frame being shown; headless, the DirectDraw primary (which can tear: `REPORT.md`) |

The spike's options are unchanged: `--game DIR` (required), `--run`, `--headless`, `--trace`
(every import call on stderr as `[hle] name`) and `--watchdog S`. Once a second the host prints
`[fps]`: frames shown (windowed) and primary-surface updates. On a host fault it prints the image
load address for `atos -o build/sfcop -l <load> <pc>`.

## DS3D: accepted, not applied

The game positions every 3D sound. These calls are answered and their values stored, so the
matching `Get...` calls return them, but the mixer plays each sample at its own volume and pan.

| Interface | Methods | What is still to do |
|---|---|---|
| IDirectSound3DBuffer | SetPosition, SetVelocity, SetMinDistance, SetMaxDistance, SetMode, SetAllParameters | distance attenuation and pan from position relative to the listener |
| IDirectSound3DBuffer | SetConeAngles, SetConeOrientation, SetConeOutsideVolume | cone attenuation |
| IDirectSound3DListener | SetPosition, SetOrientation, SetVelocity, SetAllParameters | the frame the buffers are placed in |
| IDirectSound3DListener | SetDistanceFactor, SetRolloffFactor, SetDopplerFactor | rolloff and Doppler pitch |
| IDirectSound3DListener | CommitDeferredSettings | deferred mode: settings take effect at once today |

## Stubs

Answered with a fixed result because the game's use of them needs nothing more:

| Call | Result |
|---|---|
| `WinExec` | 2 (file not found) |
| `OpenFileMappingA` | NULL, last error 2: no other instance is running |
| `VirtualQueryEx` | 0 (logged once) |
| `GetThreadContext` | FALSE |
| `IDirectInputDevice::SetEventNotification` | DI_OK, but the event is never signalled (logged once) |
| `VirtualLock` | TRUE |
| `GetSystemMenu`, `IsIconic`, `GetDIBColorTable` | NULL / FALSE / 0 |
| `ChangeDisplaySettingsA` | DISP_CHANGE_SUCCESSFUL; DirectDraw owns the mode |
| `GetOpenFileNameA/W`, `GetSaveFileNameA/W` | FALSE (cancelled) |
| `RegisterEventSourceA`, `ReportEventA`, `DeregisterEventSource` | a dummy handle; the report goes to stderr |
| `CommDlgExtendedError` | 0 |
| DBGHELP: `SymInitialize`, `SymGetOptions`, `SymSetOptions` | TRUE / 0 / the options passed |
| DBGHELP: `SymLoadModule`, `SymGetSymFromAddr`, `SymGetModuleInfo`, `SymGetLineFromAddr`, `SymFunctionTableAccess`, `StackWalk` | 0 / FALSE: the crash reporter finds no symbols and no frames |

Implemented: `GetShortPathNameA` (the long name), `MapViewOfFileEx` (as `MapViewOfFile`),
`EnumDisplaySettingsA` (win32hle's DirectDraw modes at 8, 16 and 32 bpp), `VkKeyScanA` (US
layout), `SystemParametersInfoA` (work area; accessibility features off), `ExtTextOutA` (as
`TextOutA`) and `DirectDrawCreateClipper`.
