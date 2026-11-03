# Mac baseline: untouched 2.5.5.2 under Wine (task P0-5)

Date: 2026-11-02. Machine: MacBook Pro M1 Pro, 32 GB, macOS (Darwin 27.0), display 2560x1440 points.

## Result

| Stage | Status | Evidence |
|---|---|---|
| Wine installs, prefix boots | yes | `wineboot -i` exit 0; prefix `work/wine/prefix` |
| `StarFleetOP.exe` starts, Direct3D initialises | yes | process alive after 120 s; ~3,500 Direct3D device trace lines in 45 s (ddraw trace) |
| Picture on screen | **yes with fullscreen inside a Wine virtual desktop**; windowed is black | `B_gl_fullscreen/shot_90s.png` vs `A_gl_windowed/shot_120s.png` |
| Loading screen | yes, shows version `2552` | `B_gl_fullscreen/shot_90s.png` |
| Main menu | reached once after the CD key was entered (2026-11-03), then **startup crashes** (below) | `C_after_key/shot_15s.png`..`shot_60s.png` |
| Skirmish | not reached (behind the CD key) | - |
| Crashes | none before the key; after it, every boot since the first crashed 2-12 s after launch, except one traced run | `E_with_scr/crash_excerpt.txt` |

The game boots and renders on this Mac. It stops at a CD-key dialog. The key is stored in the registry
under `HKLM\SOFTWARE\WON\CDKeys` (string `SOFTWARE\WON\CDKeys` at file offset `0x488e50`; error strings
"CD key is invalid", "CD key is already in use"). Our prefix is a plain folder copy, so no installer
ever wrote the key. A human has to type it once (`oracle/launch_game.sh`); after that it is in the prefix's
registry and later runs pass without input. The agent did not enter the key and did not look for one.

## Working configuration

| Setting | Value |
|---|---|
| Wine | Gcenx `macOS_Wine_builds` release `11.0_1`, `wine-stable-11.0_1-osx64.tar.xz` (sha256 `b50dc50ec7f41d58b115a6b685d4d1315ba3c797bd3aa0f49213f2703cb82388`), reports `wine-11.0`; x86_64 host binaries under Rosetta 2, wow64 with `i386-windows` PE DLLs, so no separate 32-bit prefix is needed |
| Location | `tools/wine/stable-11.0_1/Wine Stable.app` (681 MB), prefix `work/wine/prefix` |
| Game copy | `C:\Games\SFCOP` in the prefix, rsync of `reference/installed game/...`; `StarFleetOP.exe` sha256 `264da726…c883` checked against `analysis/results/retail_hashes.sha256` |
| Executable | `StarFleetOP.exe` directly (`SFCOP.exe` is the boot stub) |
| `sfc.ini` `[3D] windowed` | `0` (fullscreen). `1` renders but presents only black |
| Wine virtual desktop | on, `800x600` (`HKCU\Software\Wine\Explorer` `Desktop=Default`, `...\Desktops` `Default=800x600`) |
| Direct3D renderer | Wine default (wined3d over OpenGL); not changed |
| DLL overrides | none |
| Registry | none besides the desktop keys (the exe reads no install-path keys; only `software\mpath\mplayer\main`, `SOFTWARE\GameSpy\CDKeys`, `SOFTWARE\WON\CDKeys`, `SOFTWARE\Quicksilver\Debug`) |

`oracle/setup_prefix.sh` reproduces all of it; `oracle/baseline/boot_test.sh <label> [seconds] [every]`
reproduces each boot test below (env `RENDERER`, `WINDOWED`, `DESKTOP`).

## Boot tests

| Label | Renderer | windowed | Virtual desktop | Run | Outcome |
|---|---|---|---|---|---|
| `A_gl_windowed` | default (GL) | 1 | 800x600 | 120 s | Alive, black. `A_gl_windowed/wine_40s_first_try.log` is an earlier identical 40 s run |
| `A_gl_windowed_ddrawtrace` | default (GL) | 1 | 800x600 | 45 s, `WINEDEBUG=trace+ddraw,trace+seh` | Alive, black; renders through Direct3D with a clipper (`SetCooperativeLevel` flags `0x8` = `DDSCL_NORMAL`); the 2 MB trace was summarised here and deleted |
| `B_gl_fullscreen` | default (GL) | 0 | 800x600 | 90 s | Loading screen, then CD-key dialog |
| `D_no3d` | `no3d` (wined3d without 3D) | 0 | 800x600 | 90 s | Game exits within 90 s (`Disabling 3D support`); it needs Direct3D |

The game uses the DirectX 6 interfaces (`IDirectDraw4`, `IDirect3D3`/`IDirect3DDevice3`; task P0-4).
Names such as `IDirect3DDevice7` in Wine traces are Wine's internal implementation names, not what the
game asked for; the ddraw trace also shows `ddraw4_*`, `d3d_device3_*` and `ddraw_surface4_*` entry points.

Only Wine error in every run: 5-6 × `err:d3d:wined3d_check_gl_call GL_INVALID_FRAMEBUFFER_OPERATION from
glClear` (`dlls/wined3d/context_gl.c:2245`) at start-up. It did not stop rendering in fullscreen mode.
The one SEH exception in the trace is `RPC_S_SERVER_UNAVAILABLE` (0x6ba) in a Wine-internal thread,
handled.

Not tried, because fullscreen already works: `RENDERER=vulkan` (wined3d over MoltenVK), no virtual
desktop, DXVK or dgVoodoo-style ddraw wrappers.

## After the CD key (2026-11-03)

Marino entered the key with `launch_game.sh`; `export_cdkey.sh` saved it (one value,
`StarfleetCommand2`, REG_BINARY, under `HKLM\SOFTWARE\GameSpy\CDKeys`, 32-bit view). The next boot,
`C_after_key`, showed the main menu ("Select Game") from 15 s on. Every boot after that crashed:

| Run | Change | Outcome |
|---|---|---|
| `C_after_key` | none | main menu at 15 s, alive at 75 s |
| `cal-1` (`run_oracle.sh`) | `OracleSkirmish.scr` installed | no window within 45 s |
| `E_with_scr` (x2) | same | dead within 45 s; access violation |
| `F_without_scr` | our `.scr` removed | dead within 45 s |
| `G_no_lst` | also the game's `Assets/Scripts/scripts.lst` cache moved aside | dead |
| `H_ini_restored` | also `sfc.ini` back to retail (the game had set `voice=0`, `MusicVolume=100`) | dead |
| 4 plain launches | none | dead after 6, 4, 6, 12 s |
| 3 plain launches | Wine audio driver disabled | dead after 2 s each (reverted) |
| 1 launch, `WINEDEBUG=+thread` | slower | alive at 25 s (killed) |

The crash: a secondary thread faults in Wine's 32-bit `kernel32.dll` (0x7BF2123D, reading 0x4DC9),
its SEH chain points outside its stack, and Wine cannot dispatch the exception
(`E_with_scr/crash_excerpt.txt`). Meanwhile the main thread is still loading and unloading the
shipped `Assets/Scripts/*.scr` one by one, and gets to a different script each time, so it is a
race. The game files are back to retail except the two caches the game writes itself
(`scripts.lst`, `Above8BitTextures.txt`); the registry gained only the key. Before the key the game
waits at the key dialog and never gets this far, which is why A/B ran 90-120 s without a crash.
Not yet tested: whether the GameSpy network start-up (`getaddrinfo` fails to resolve the host name
in every run) or the Mac display state (whether the window is on the visible Space) is the
trigger; `RENDERER=vulkan`; a newer Wine or CrossOver.

## Notes

- macOS showed a notification that "Wine Stable" contains a component that will not open in macOS 28
  (most likely the Rosetta 2 dependency). It still runs on this macOS; a later macOS may need a newer
  Wine build or CrossOver.
- Screenshots: `oracle/shot.sh` captures only the game's own macOS window (owner `wine`, title
  "Starfleet Command", found with `CGWindowListCopyWindowInfo`, then `screencapture -l`). The `A_` and
  `B_` shots were taken with a fixed screen region (`screencapture -R0,30,800,600`); a later region
  shot recorded another app that was on top, was deleted, and region capture was removed.
  The window must be on the visible Space: when it is not, macOS does not composite its GL content and
  the shot is black apart from the cursor. Inside Wine, GDI capture of the game window (`oracle_input`
  `shot`) is always black for the same reason.
- Not measured yet: frame rate, audio, input latency. The oracle does not need any of them.

## Next steps

1. Someone with the key runs `oracle/launch_game.sh` once and enters it, then quits.
2. Rerun `WINDOWED=0 oracle/baseline/boot_test.sh C_after_key 120 30` with the game window on the
   visible Space to confirm the main menu, then calibrate `oracle/driver/skirmish.steps` (see
   `oracle/README.md`).
