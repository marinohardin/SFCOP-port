# oracle/driver: steps files and the programs that play them

A steps file is a menu path written once and played by two drivers: `oracle_input.exe` into the
original under Wine, and the native client's own player (`sfcop --input`, `src/app/input/`). The
vocabulary and what each driver does with each step are in `src/app/input/README.md`, "Steps";
`oracle_input.c`'s header lists the Wine side.

| File | What |
| --- | --- |
| `oracle_input.c`, `build.sh` | The Wine driver: a Win32 program built by VC6, run in the game's prefix (`oracle/README.md`) |
| `oracle_launch.c`, `oracle_clock.c` | The fixed-step clock under Wine: `oracle_launch.exe` starts the game with `oracle_clock.dll` in it (below) |
| `oracle_clock_check.c`, `oracle_clock_check.expected`, `test_clock.sh` | The test of that clock under Wine, without the game |
| `skirmish.steps` | The Wine path towards the oracle skirmish, in the game's display pixels (800x600, the 640x480 menu centred); partly calibrated |
| `probe/probe.steps` | Wine only: lists the windows the driver can see |
| `menu.steps` | Native: boot to the main menu ("Select Game") and click SINGLE PLAYER, which opens `Setup`; in the game's display pixels |
| `campaign.steps` | Native: boot to the main menu and play a single-player campaign start to the mission's combat (`analysis/campaign/README.md`); includes the map's compass recentre step |
| `run_native.sh` | Plays a steps file into the native client with a prepared data folder |

## The native runner

    SFCOP_ORACLE_DATA=<prepared data folder> oracle/driver/run_native.sh

runs `sfcop --data <folder> --windowed --input oracle/driver/menu.steps --watchdog 90` (90 s, not 30, until the boot time is fixed), and
`--headless`, `--fullscreen`, `--steps FILE`, `--watchdog S` and `-- <more sfcop flags>` change
it. The data folder must already hold a stored key, so the boot goes past the key prompt; agents
never make, read or list one. Its path comes from `SFCOP_ORACLE_DATA`, or from the first line of
the file `SFCOP_ORACLE_CONFIG` names (default `~/.config/sfcop/oracle-data`), both outside git. The
script starts with `set +x`, never prints the path, and reads nothing under it; without it, it
stops with exit 2. `SFCOP_BIN` picks the client (default `build/macos-arm64/sfcop`) and
`SFCOP_INSTALL` its `--install`.

The exit is the client's: 0 when the watchdog finds its goal mark (with a stored key, the main
menu, "Select Game"), 1 when not, 2 for a bad steps file or a wait in it that ran out.

## menu.steps

Waits for the `Select Game` mark (the entry of `tState_Namespace_Setup::tState_SelectGame`, the
main menu), lets the menu settle, then clicks SINGLE PLAYER at (237, 232) and waits for the screen
it opens: the `Setup` mark, the Single Player screen (the entry of
`tState_Namespace_Setup::tState_Setup`). A click that does not take ends the run with exit 2
after 10 s. The menus run in an
800x600 display mode with the 640x480 menu centred, the same space as the Wine driver's client
area, so the coordinates are the ones `skirmish.steps` has. `waitmark` is the native player's: the
Wine driver refuses the line, so this file is for the native client only.

## The fixed step under Wine

The native client runs `--oracle` and `--record-trace` on a fixed-step clock: the game's ticks
and counter grow by a fixed step per frame, never by the wall clock (`src/app/oracle/README.md`,
"The fixed step"). The original is recorded with the same step, or the two traces cannot agree.
`oracle_launch.exe` does that for the original:

    wine oracle_launch.exe [--fixed-step MS] StarFleetOP.exe

starts the program suspended, loads `oracle_clock.dll` (from the launcher's folder) into it, and
has it replace eight entries of the exe's import table: `GetTickCount`, `QueryPerformanceCounter`,
`QueryPerformanceFrequency` and `Sleep` (KERNEL32), `timeGetTime`, `timeSetEvent` and
`timeKillEvent` (WINMM) and `PeekMessageA` (USER32). Then the game runs, and the launcher exits with its exit code. MS is the native flag's
(`--fixed-step`), 16.667 by default, as the native oracle's. The rules are the native clock's:

| Native client (`FixedStepClock`) | Under Wine (`oracle_clock.dll`) |
| --- | --- |
| ticks and the 10 MHz counter from 0 | the same |
| one step per pump turn that read no window event | one step per `PeekMessageA` on the main thread that found no message |
| `Sleep` on the main thread adds what it asks | the same |
| 10,000 reads on the main thread with no growth add 1 ms | the same |
| other threads' sleeps and reads add nothing | the same |
| multimedia timers have no thread: due by the clock, they fire on the main thread after the frame's step, earliest due first, a tie by the lower id | the same, inside the `PeekMessageA` that found no message |

Where they can part: a frame is counted natively before the game's queue is read and under Wine
after, so a message the game posts to itself makes a frame natively and none under Wine (none of
the oracle's runs is known to do that); a blocking `GetMessageA` (the game in the background)
counts a frame per 5 ms wait natively and none under Wine; and a mission module's own imports are
not replaced, only the exe's. The game's 25 ms multimedia timer (`timeSetEvent`) fires on both
sides by the same rule, so its callback runs at the same frames (`src/app/oracle/README.md`, "On
the fixed step", for what that changed natively).
`test_clock.sh` checks the launcher and the DLL in a prefix of its own, with a program that
reads the clock as the game does; its readings are written by hand in
`oracle_clock_check.expected`:

    SFCOP_ROOT=/path/to/SFCOP sh oracle/driver/build.sh
    oracle/driver/test_clock.sh            # test_clock.sh: ok

### Recording the baseline with it

`oracle/run_oracle.sh` records on the fixed step when the environment variable
`ORACLE_FIXED_STEP` holds the step in milliseconds; it passes it to the launcher's flag,
`--fixed-step MS`. To re-record the baseline at the native oracle's default step:

    SFCOP_ROOT=/path/to/SFCOP sh oracle/driver/build.sh
    ORACLE_FIXED_STEP=16.667 oracle/run_oracle.sh <label>

With it set, the script copies `oracle_launch.exe` and `oracle_clock.dll` from
`work/oracle/build/out/` beside the exe and starts the game as
`wine oracle_launch.exe --fixed-step 16.667 StarFleetOP.exe`; everything else in the run (the
script, `oracle_input.exe`, the wait for `END`, the hash) is unchanged, and `run.txt` gains
`fixed_step`, `launch` and `clock` lines (the step and the two programs' hashes). If either
program is missing it refuses before starting anything (exit 2) rather than record a run that is
not pinned. Unset, the script runs exactly as before, on the wall clock. The two programs are
built from their source in this folder (`oracle_launch.c`, `oracle_clock.c`) by `build.sh`; they
are never committed.

The launcher's line (`oracle_launch: fixed step 16.667 ms, 8 imports replaced`) is the first of
the run's `wine.log`; a run without it was not on the fixed step. A baseline recorded at another
step is compared only with native runs given the same `--fixed-step`.

**What a frame is, on each side.** Under Wine the DLL counts a frame when `PeekMessageA` on the
main thread returns no message: the point where the game's loop finds its queue empty and runs a
frame. The native pump uses the same event as nearly as the host allows: a pump turn that read no
event from the window, after which the game's `PeekMessageA` finds the queue empty. They are not
the very same call, because natively the frame is counted in the host's pump before the game reads
its queue, and under Wine inside the game's own call after it. They differ in two cases: a message
the game posts to itself (natively a frame, under Wine none; no oracle run is known to do this),
and a blocking `GetMessageA` while the game waits in the background (natively a frame per 5 ms
wait turn, under Wine none). So a native trace and a Wine trace on the same step are comparable
sample for sample while the game runs its frame loop in the foreground, which is every sample of
the skirmish; a run that leaves that loop is compared only as far as the first sample after it.

`oracle_input.exe` itself stays on the wall clock: its `sleep` steps are real milliseconds, while
the native player's are the fixed-step clock's. A step's time is then a delivery order, not a time;
`src/app/oracle/README.md`, "The fixed step", says what that means for the comparison.
