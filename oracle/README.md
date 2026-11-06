# Replay oracle (task P0-5)

The oracle is the behavioural judge for every rebuilt, relinked or lifted version of the game. A
mission script runs a fixed two-ship skirmish, logs per-sample ship state and every game event to a
text file, and a driver hashes the normalised log. A recording made against the untouched 2.5.5.2
exe is the reference; a candidate build passes when it produces the same hash, and
`compare_traces.sh` shows the first sample and field where it differs.

**Status (2026-11-03):** Wine baseline done (`BASELINE.md`): the game boots and renders, and stops
at a CD-key prompt that a human must answer once. The mission script builds with the P0-2/P1-8
toolchain (`toolchain/build_scr.sh`): all sources compile without warnings, and the link gives
`OracleSkirmish.scr` (339,968 bytes) exporting `MissionSpecEntry` and `ScriptEntry` like every
shipped script. The input driver builds and runs inside the prefix and finds the game's window.
The hashing/compare path is dry-run tested (`tests/selftest.sh`). **No trace recorded yet.** The CD key
has been entered and is saved privately ("CD key" below); the game then reached its main menu once,
and since then crashes in Wine's OpenGL set-up a few seconds after launch (`BASELINE.md`, "After
the CD key"). The click path is calibrated up to SINGLE PLAYER. `BASELINE.md`, "How to record the
oracle elsewhere", covers CrossOver, another Wine build, or a Windows PC.

**Presets (2026-11-05):** `presets/` runs the oracle on the lifted client of `macos-arm64`, `linux-x64` and `windows-x64` and compares the traces; the menu path compares today, the skirmish when A2's lands. Cost per pull request that touches `game/` or `src/`: about 2.3 minutes with the runner's variables set (macOS 63 s, Linux 62 s, Windows skipped), 8 s while they are not (`presets/README.md`).

## Files

| Path | What |
|---|---|
| `wine-env.sh` | Sourced by everything: Wine path, `WINEPREFIX`, game folder in the prefix |
| `setup_prefix.sh` | One-time: create the prefix, copy the game in (hash-checked), fullscreen + 800x600 virtual desktop |
| `launch_game.sh` | Start the game for a human (first-run CD key, calibration); afterwards saves a newly stored key with `export_cdkey.sh` |
| `export_cdkey.sh`, `import_cdkey.sh` | Save the CD key from the prefix's registry to `work/private/`, and put it back into a fresh prefix (see "CD key") |
| `baseline/boot_test.sh` | Boot with one graphics configuration, screenshot the game window, kill. Results in `baseline/<label>/` |
| `baseline/survive.sh <label>` | Start-up crash test: how long each of N launches stays alive; appends to `baseline/survive.tsv` |
| `shot.sh out.png` | Screenshot of the game's own macOS window only (never a screen region) |
| `scr/OracleSkirmish/` | Mission script source (VC6, SDK `OP API - R2.1`), plus `OracleSkirmish.dsp` mirroring the SDK template |
| `scr/build.sh` | Build `OracleSkirmish.scr` with `toolchain/build_scr.sh`; output `work/oracle/build/out/` |
| `driver/oracle_input.c`, `driver/build.sh` | Win32 helper that clicks through the menus from a steps file, runs in the same prefix |
| `driver/skirmish.steps` | The click path. **Uncalibrated placeholders** until the menu has been seen |
| `driver/probe/probe.steps` | No-input probe: lists the windows the driver can see |
| `driver/menu.steps` | Native client: boot past the key prompt to the main menu and click SINGLE PLAYER (`driver/README.md`) |
| `driver/run_native.sh` | Plays a steps file into the native client (`sfcop --input`) with a prepared data folder whose path comes from configuration and is never printed (`driver/README.md`) |
| `presets/` | The oracle on each preset's build and the comparison of the traces (`presets/README.md`): `run_preset.sh`, `compare_presets.py` |
| `driver/README.md` | Steps files, the two drivers that play them, and the native runner |
| `oracle.sh [label]` | Everything: build both, install, run twice (`<label>-1`, `<label>-2`), compare |
| `run_oracle.sh [label]` | One oracle run: install `.scr`, start game and driver, wait for `END`, kill, normalise, hash. Output `traces/<label>/` |
| `normalize_trace.sh` | Drops `#` lines, CRLF, trailing blanks |
| `compare_traces.sh A B` | Same hash, or first differing line and fields |
| `tests/selftest.sh` | Dry run of normalise + compare on synthetic traces (`tests/selftest.out`) |

## How to reproduce

```sh
oracle/setup_prefix.sh                      # needs tools/wine/stable-11.0_1 (see BASELINE.md)
oracle/launch_game.sh                       # once: a human enters the CD key, then quits (key saved privately)
# calibrate driver/skirmish.steps (below)
oracle/oracle.sh ref                        # build, install, run twice, compare
```

`oracle.sh` is `scr/build.sh`, `driver/build.sh`, `run_oracle.sh ref-1`, `run_oracle.sh ref-2`
and `compare_traces.sh ref-1 ref-2`; each can be run on its own. `run_oracle.sh` copies the
`.scr` into the prefix's `C:\Games\SFCOP\Assets\Scripts\` and the driver and steps file into
`C:\Games\SFCOP\`, starts the game, starts the driver 10 s later, and waits for the trace's `END`
line.

### CD key

The game will not reach its main menu without a CD key. The agents never type, read, print or ask
for it. A human enters it once with `oracle/launch_game.sh`; when the game exits, that script runs
`export_cdkey.sh`, which saves the registry keys the game wrote to `work/private/cdkey.reg`
(`HKLM\SOFTWARE\GameSpy\CDKeys`) and `work/private/cdkey-won.reg` (`HKLM\SOFTWARE\WON\CDKeys`),
whichever exist, mode 600, and prints only the value names and types. `setup_prefix.sh` runs
`import_cdkey.sh` when a file is in `work/private/`, so a new prefix starts with the key in place.
`work/private/` is outside the repo; `SFCOP_PRIVATE` overrides the location.

Rules for these scripts, and for anyone who edits them: they read the key only inside their own
process, never echo, log or copy it anywhere but the Wine registry and the files above, and start
with `set +x` so tracing cannot print it. Agents do not open `work/private/*` or the prefix's
`*.reg` files.

What the exe does with the key (`StarFleetOP.exe` 2.5.5.2, from the disassembly; names are
descriptive): the code that saves and loads it (0x5D9F3D, 0x5DA008) opens
`SOFTWARE\GameSpy\CDKeys` and stores a buffer made by 0x5D9DB1, which Blowfish-encrypts the key
(a 0x10B8-byte cipher context, 0x6ED510/0x6ED560/0x6EE3D0). The Blowfish key is derived by
0x5D9CD3 from a product-name string only: CRC-16 (0x6E1680, table at 0x889068) of the name, then of
each of its first three characters. Nothing machine-specific goes in, so an export works in any
prefix. The registry holds the encrypted form, not the typed key, which is why a plaintext key in
`work/private/sfcop.env` cannot simply be written with `reg add`: that would need the cipher mode,
the product name and the key's binary encoding (the exe's key formats are `####-####-####-####`
with `%I64x`, and WON's `CVCNCVCNCVCNCVCNNNNN`) worked out and checked against a real export.
`import_cdkey.sh` therefore does not read `sfcop.env`. `SOFTWARE\WON\CDKeys` has no code reference
in the Ghidra export; it is exported too in case the game writes it. The game is 32-bit, so in this
wow64 prefix both keys are in the 32-bit registry view: the scripts pass `/reg:32` (checked with a
throwaway key: export, delete, import, query).

### Build

`scr/build.sh` calls `toolchain/build_scr.sh oracle/scr/OracleSkirmish <out.scr>`, which compiles
each `.cpp` listed in `OracleSkirmish.dsp` with its Release `# ADD CPP` line and links with its
`# ADD LINK32` line; both are the SDK `_ScriptTemplates/Multi` template's, with the include paths
of a script at `Scripts/<Type>/<Name>/`. The toolchain handles the SDK's truncated
`dmp3D_MSVCR.lib` (it adds `toolchain/scr/dmp3d_shim.cpp`, byte-checked against the copies in a
shipped `.scr`) and builds the export file separately (`toolchain/README.md`). About 12 s. The
`.scr`'s hash changes from build to build (most likely the PE link timestamp; not checked), so
`run.txt` records it but it is not something to compare; the trace hash is.

`driver/build.sh` builds `oracle_input.exe` with `toolchain/bin/cl` and `link` (C89, console,
`user32`/`gdi32`/`kernel32`). It uses `mouse_event`/`keybd_event` because the VC6 headers predate
`SendInput`. At start it joins the window station's `Default` desktop: started early, Wine put it
on a desktop of its own where `EnumWindows` never saw the game.

Installing Wine: download `wine-stable-11.0_1-osx64.tar.xz` from
`https://github.com/Gcenx/macOS_Wine_builds/releases/tag/11.0_1` (sha256 in `BASELINE.md`), unpack into
`tools/wine/stable-11.0_1/`, `xattr -dr com.apple.quarantine` on it.

Calibrating the click path: there is no command-line switch that selects a mission (the exe's only
switches are `-Host`, `-Client`, `-DontTerminate`, used by the GameSpy lobby launcher), so the driver
has to click. The game's window is class `SFC`, title `Starfleet Command`, 800x600 client area.
The driver's own `shot` step is black under Wine (GDI cannot read the DirectDraw/GL surface), so
calibrate with `ORACLE_SHOTS=10 oracle/run_oracle.sh cal-1`, which saves `shot.sh` captures of the
Mac window every 10 s into `traces/cal-1/mac_<t>s.png`; keep the game window on the visible Space
(macOS does not composite it otherwise and the capture is black). Add one `click x y` per screen
(client pixels; the PNGs are 2x on a Retina display), rerun with a new label, repeat. The script's
title is `!Oracle Skirmish` so it sorts first in the mission list. The steps file is an input of
the oracle: hash it (run_oracle.sh records its SHA in `run.txt`) and do not change it between a
reference and a candidate run.

## Mission design

`scr/OracleSkirmish/`, built against the SDK headers in `OP API - R2.1/Scripts`:

- `kHistoricalMission` (the type the game's Select Skirmish list is built from; a
  `kSkirmishMission` is not listed there), two teams. Team 1 is the human's slot (`kPlayableTeam`,
  tag A, Federation), team 2 is AI only (`kNPCTeam`, tag B, Klingon), at war with each other; the
  races are pinned by the spec so that nothing is drawn before the generators are pinned.
- Fixed ships, not the ones picked on the setup screen (`mCreateNewShip` is empty, custom skirmish
  disabled): team 1 `F-CA` (Federation CA, BPV 125) at map letter G, team 2 `K-D7` (Klingon D7, BPV
  126) at map letter H, `kNoShipOptions` so there is no random start offset.
- One empty "Open Space - L" map and `TotalMaps = 1`: the SDK clamps the preferred map to
  `TotalMaps`, so the setup screen's map choice cannot change anything.
- The human's ship is handed to the AI (`mSetShipAIControlled(kTrue)`, `ORACLE_PLAYER_AUTOPILOT`),
  so both ships are AI-flown and no input is needed after launch.
- All constants are in `OracleConfig.h`; bump `kOracleTraceVersion` when any of them changes.

### What is logged (`OracleTrace.txt` next to `StarFleetOP.exe`)

`tOracleRecorder` is a `tActor` registered with the SDK's event manager. It overrides
`mHandleShipEvent`, so it sees every event the game sends to the script before any state filtering.
It samples on a `kTimerTurnsSet` timer (game turn time, not wall-clock seconds) every
`kOracleSampleTurns` = 1/32 turn, for at most 640 samples (20 turns), or until 32 samples after one
side has no ship left.

```
# oracle-trace v1                                  metadata, not hashed
# rng pin 1 crt_srand=0x007385DE lcg_seed=0x0088412C
# shipmap id<entity> c<customID> team <t> class <spec> bpv <n>
S <sample> c101 grid <x> <y> raw <gx> <gy> <i0..i4> <f0..f4> spd <n> dmg <pct> tgt c201 dist <d> st <bits>
E <sample> <event-bit-hex> c101 i <i0..i4> f <f0..f4> h c201
END <sample> <reason>
```

- **S** records, per ship per sample: 1-based grid cell (`mGetGridPosition`), the raw
  `tShipSystemsInfo` the game returns for `kGetShipPosition` (pre-filled with sentinels, so any field
  the game fills beyond the grid cell shows up; this is how we find out whether finer position or
  heading is available), speed (`mGetShipSpeed`), damage percent, current target, tactical distance to
  the enemy ship (a float, the finest position signal the API has), and the script-side status bits
  (destroyed, captured, disengaged...).
- **E** records: every event, in arrival order. Damage events (`kNotifyDamage`, `00000080`) carry the
  hostile ship and damage type. Shield, hull and engine values arrive as `kNotifyShipSystemUpdate`
  (`01000000`) replies to the requests made each sample for the shield, hull and engine groups
  (`mGetHealthOfSystem`; the game answers it only for a group hard point: a single system's, such as
  one shield or armor, reads a null entry of the ship's group table and ends the game).
- Ships are named by script custom ID (`c101`, `c201`), not by the game's entity ID, so the trace
  does not depend on entity numbering.
- Floats are printed with `%.9g`, which round-trips a 32-bit float exactly.

### What the scripting API cannot see

No world coordinates (only the grid cell, unless the raw position reply has more), no heading, no
per-weapon fire events (only "damaged by ship X"), no energy allocation. Heading and finer position
can be approximated from grid cell + distance over time. If the first real trace shows the raw reply
is just the grid cell, the next step is to read the ship object from game memory via the P0-3
class layout, which makes the oracle depend on the exe's layout.

## Determinism assumptions

The trace is only useful if two runs of the same exe produce the same hash. Known and suspected
sources of variation:

1. **Random number generators** (found by byte search in 2.5.5.2, `oracle/scr/OracleSkirmish/OracleSeed.cpp`):
   - the static MSVC 6 CRT `rand()` at `0x007385EB` (state per thread in `_getptd()+0x14`), 111 call
     sites; `srand()` at `0x007385DE` is called from 9 places: seeded from `timeGetTime()` (call at
     `0x004FCB8F`), from `0x007390FA(0)` (very likely `time(NULL)`; at `0x005098C9`, `0x006C2267` xor
     `0x33333333`, `0x006DE5BB`), from `GetCurrentThreadId()` (`0x0073325F`), from `0x006BF540()`
     (`0x006BA026`, `0x006C1A22`, unidentified), from a seed member of some object (`0x0066C1CC`), and
     one site that did not disassemble cleanly (`0x004FC837`).
   - an inlined LCG with the CRT constants, seed at `0x0088412C` (static initial value 1, written
     only by itself), returning a float, 93 call sites.
   The script pins both at mission start and again at sample 0: it finds them by byte pattern, not by
   address, calls the game's own `srand` and writes the LCG seed. A relinked build with the same CRT
   objects is pinned too; a native rebuild must offer an equivalent hook or the oracle will diverge
   on the first die roll. Not yet known: whether the engine reseeds after sample 0 (then the hash will
   not repeat and the `srand` call sites above are where to look), and whether other generators exist
   (two hits of the constant `0x3039` at `0x0072A1D3` and `0x0087B426` are unexamined).
2. **Timer dependence.** Samples are taken on game-turn time, so sample points follow the
   simulation, not the wall clock. Whether the simulation itself is fixed-step is unknown. If it
   integrates with frame-time deltas (`timeGetTime`), positions will differ between runs at the float
   level, and the oracle will need either a fixed-rate clock (patch or hook `timeGetTime` in the
   prefix) or coarser comparison. The first two-run comparison answers this.
3. **Rendering-coupled randomness.** The LCG has many call sites that look visual (explosions,
   particles). If any of them also feeds the simulation, frame rate changes the sequence. Pinning at
   sample 0 does not help after that point.
4. **Event delivery.** Events are one-shot in the SDK (`tEventMgr::mEntityEvent` clears the bit after
   delivery), so the recorder re-arms before every request. If system replies arrive asynchronously,
   only the first reply per sample may get through; the trace will show it, and the request should
   then rotate one system per sample.
5. **Timer semantics.** Whether a turn timer repeats is undocumented; the recorder kills and re-arms
   it on every tick, which is correct either way. Whether fractional turns (1/32) are honoured is
   unverified; the header records the value.
6. **The teams' races** are pinned by the spec (`kSpecFed`, `kSpecKling`): with a choice of races
   the game drew each team's race before the script pinned the generators, and the draw changed
   the AI's behaviour from run to run.
7. **Excluded from the hash:** `#` lines (addresses, entity IDs, races chosen on the setup screen),
   CRLF, trailing blanks.

## Comparing two runs

`compare_traces.sh A B` prints both hashes. If they differ it prints the first differing line, its
sample number, each field that changed, and the number of differing lines. A trace that ends in
`END <n> aborted` or has no `END` (timeout) is incomplete and `run_oracle.sh` exits non-zero.
Reference traces belong in `traces/` with the `run.txt` that records Wine version, exe, `.scr` and
steps-file hashes.
