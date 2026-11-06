# src/app/oracle: the replay oracle on the native client

`sfcop --oracle TRACE --input SCRIPT [--logs DIR] [--headless] [--watchdog S]` plays the oracle
mission on the lifted game and compares its trace with `TRACE` (DESIGN §5.10, §6.5, §6.8). The
same run with `--record-trace OUT` instead of `--oracle` writes the trace without comparing.

| File | What it does |
| --- | --- |
| `OracleRun.h` | The names a trace path carries, the wait for the mission log's `END`, the trace made by `oracle/trace/mklog.py`, the comparison by `compare.py`, the trace header's exe hash and build text |
| `OracleRunTest.cpp` | Doctests: the names, the END wait against a fake clock, the exit codes on C5's fixtures (`oracle/trace/fixtures/`, invented), the header values |
| `../main.cpp` | Starts the run on a thread of its own beside the game |

## How a run goes

1. The input script (`oracle/driver/skirmish.steps`) boots to the menu, opens Single Player,
   picks the oracle mission and starts it; its last step waits for the mission's log.
2. The mission writes `OracleTrace.txt` beside the exe. The kernel module puts a `.txt` the game
   writes beside the exe under `logs/` of the per-user folder (`src/hle/kernel/README.md`);
   `--logs DIR` places that folder at `DIR` instead (the kernel module's `LogsStore`), so a run on
   a prepared data folder reads nothing in it: the log is the one file that leaves it.
3. The oracle thread polls the log every 250 ms. When its last line is an `END` record (the
   recorder closes the file right after writing it), the thread hashes the install's exe, asks
   `git` for the source tree's commit, runs `mklog.py` with `--mission` and `--input` taken from
   the trace path (`<root>/<mission>/<input>.trace`, as `oracle/expected/README.md` lays them
   out) and `--build "native <preset> <commit>"`, and writes the trace at
   `<logs>/<mission>/<input>.trace` (or `OUT`).
4. With `--oracle`, `compare.py EXPECTED ACTUAL` runs and the process ends with its status:
   0 identical, 4 a tick differs (the first is printed), 2 a malformed trace. A log `mklog.py`
   refuses ends the run with 2.
5. The oracle owns the deadline: `--watchdog S` (a day without it) ends a run whose `END` never
   came with exit 1. The guest host's own watchdog is off on an oracle run, because it counts a
   run that reached the menu as a success.

The process ends as the watchdog and the input script's failure end it, with `_Exit` from the
oracle's thread: the game's threads are not unwound.

The trace tools are `oracle/trace/` of the source tree the client was built from (the build config
names that tree through its hash list), run with `python3` through the shell; `SFCOP_ORACLE_TOOLS`
and `SFCOP_PYTHON` override them. The translated missions come from `$SFCOP_MISSION_MODULES`
(`tools/scr2native/README.md`).

## In `scripts/check.sh`

The replay oracle block plays every `oracle/expected/<mission>/<input>.trace` on
`build/macos-arm64/sfcop` with `oracle/driver/<input>.steps`, headless, `--logs` under its
temporary folder and `--data` the prepared folder named by `SFCOP_ORACLE_DATA` (or the first line
of the file `SFCOP_ORACLE_CONFIG` names, default `~/.config/sfcop/oracle-data`), then
`compare.py --tree oracle/expected <logs>`: exit 0 on a match, 4 on a difference, the client's own
status when it could not play (`SFCOP_ORACLE_WATCHDOG`, default 900 s). Without a trace, a client
or a prepared folder the step skips, saying which. `tests/test_check_oracle.py` runs the block on
C5's fixture pair with a stand-in client.

## Proof runs

On a MacBook Pro (M1 Pro), the lifted build of this branch, the install, the translated missions
(`SFCOP_MISSION_MODULES`) with the oracle mission translated at base `0x66000000`, the integrator's
prepared data folder (`--data`, passed by path, holding the owner's copy of `OracleSkirmish.scr`
beside the shipped scripts), one run at a time. The steps file is `oracle/driver/skirmish.steps`.
Times are from launch; the mission starts at 71 s (SINGLE PLAYER at 51 s, SKIRMISH at 59 s, the
first list row at 67 s, which selects and starts the oracle mission, PLAY GAME at 71 s).

| Run | Mode | END | Exit | Trace (SHA-256 of the `.trace` file) |
| --- | --- | --- | --- | --- |
| `headless1` | `--headless --record-trace` | `END 640 max_samples` at 776.8 s | 0 | `5b3dfb722e996c28c9014590fee1d20439cb56386e33e935564e3388655cbe44` |
| `headless2` | the same binary, `--headless --record-trace` | `END 640 max_samples` at 771.6 s | 0 | `2dc68f000af124629ca5dd8e9b749699c61206728db26b7b850ef2aa7e8e6ffe` |
| `win2` | `--windowed --input` alone, trace by `mklog.py` afterwards | `END 640 max_samples` | 0 at the watchdog | `d60ef9ee5c9f37b31bc5bcee9d187b1a32a95752e7e6866f4abfbdfb931198ef` |
| `win3` | after a rebuild of the client and of the mission (which now ends itself), `--windowed --input` alone | `END 640 max_samples`, then the Mission Debriefing screen | 0 at the watchdog | not compared: the owner clicked in the game's window during this run, so its trace carries a human command |
| `win4` | the same, nobody at the window, through DONE on the debriefing (clicked at 1290 s) | `END 640 max_samples` | 0 at the watchdog | `63799311a36f937fd772c14e3766b18a9abc266104fbc94aaeb71d6b991fa8ee` |
| `win5` | the same again, with window captures: the Mission Debriefing after END, and the Select Skirmish screen of the menu after DONE (clicked at 1277 s) | `END 640 max_samples` | 0 at the watchdog | `ca36f3d27b9834e811c9b8076731c8311c9a9748349078f218a016a5dfc6f0b1` |

Every run: the `# rng pin` line reads `crt_srand=0x007385DE lcg_seed=0x0088412C` (both generators
found by byte pattern in guest memory and pinned, at mission start and again at sample 0), no
fault, no "nothing answers" line, 641 ticks in the trace, and the trace header
`build native macos-arm64 <commit>`.

**The traces differ from run to run (clause 3 of the package is not met).** `compare.py` on
`headless1` and `headless2` stops at tick 1; on `headless1` and `win2` at tick 0. The records are
the same in kind (the ships leave the same grid cells at the same samples, the same damage events
fall within a sample or two of each other, the player's ship is destroyed at sample 628, 629 or
634) and differ in every float from the first sample on: the first sample's tactical distance is
`318.31543` in both headless runs, `318.314667` in `win2`, `318.315399` in `win4` and `318.315338`
in `win5` (the rebuilt client, which `compare.py` also stops at tick 0 against `headless1`), and
the two headless runs part at sample 1 (`318.309631` against the other's value). The player's
ship is destroyed at sample 628, 634, 629 or 613. That is `oracle/README.md`, "Determinism
assumptions" 2, confirmed: the simulation integrates frame time, so the frame rate and its jitter
reach the trace. A fixed-rate clock for oracle runs is a follow-up; it would have to pin what the
game reads for time: `timeGetTime` (WINMM), `GetTickCount`, `QueryPerformanceCounter` and
`QueryPerformanceFrequency` (KERNEL32), and `time` through the C runtime, and advance them by a
fixed quantum per pumped frame; and the original's trace under Wine would have to be recorded
under the same step before a tick-for-tick comparison means anything.

**The compare path, run for real.** A trace recorded headless (`--record-trace`), then `--oracle`
against it: headless exit 4 (`END` at 772.3 s, `compare.py exit 4`, first difference at tick 0),
windowed exit 4 (END at 775.3 s, the same first difference), and `scripts/check.sh` with that trace
copied under `oracle/expected/` (not committed) exit 4 with `OracleSkirmish/skirmish.trace: first
difference at tick 0`. A first attempt of the check ended with exit 2: the guest host makes the data
folder the working directory, so the relative expected-trace path no longer named a file when
`compare.py` ran; every path the oracle uses is now made absolute before the game starts
(`absoluteFrom`).

The captures of each screen, taken by window id with `screencapture -l`, are in the pull request.
