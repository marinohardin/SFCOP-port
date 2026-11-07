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

## The fixed step

The simulation integrates frame time, so a run on the wall clock gives a different trace each time
(the proof runs below, before the fixed step). `--oracle` and `--record-trace` therefore run the
game on a fixed-step clock (`platform/Clock/FixedStepClock.h`), and `--fixed-step MS` puts any run
on one, for diagnosis; the client says so on stderr: `[clock] fixed step, 16.667 ms per frame`.

- `GetTickCount`, `timeGetTime`, `QueryPerformanceCounter` and `QueryPerformanceFrequency` answer
  from it (kernel32rt's `Time`): ticks and counter start at 0, the counter runs at 10 MHz.
- They grow by the step once per frame: a turn of the guest host's pump that read no event from
  the window (`src/app/guest/README.md`, "Threads"), after which `PeekMessageA` finds the queue
  empty and the game runs a frame. A turn that read an event is not a frame, so the window's own
  events (shown, exposed, focus) leave a windowed and a headless run with the same count.
- A `Sleep` on the game's main thread grows them by the milliseconds asked (and sleeps that long),
  so the game's own waits end. A `Sleep` on another thread only sleeps: those threads run as the
  scheduler lets them, and counting their waits would bring the wall clock back.
- 10,000 reads on the main thread with no growth move them by 1 ms, so a loop that waits for
  time to pass without pumping or sleeping ends instead of hanging. Each such step is counted and
  the oracle prints the count at `END`: `[clock] F frames, S spin steps, T ms on the game's clock`.
  A run with spin steps is deterministic only as far as the number of reads is.
- The time of day (`GetLocalTime`, `GetSystemTime`, `time()`) stays the host's, which the boot
  probe's fake clock shifts. Timed waits (`WaitForSingleObject` with a timeout) stay on the
  kernel's real clock.
- The multimedia timers (`timeSetEvent`) start no thread: they fall due by the fixed-step clock
  and fire on the main thread right after the frame's step, the earliest due first
  (`hle/kernel32rt/Timers.h`); the oracle prints how many at `END`:
  `[clock] multimedia timers on frames, N callbacks on frames`.

**The input script's time moves onto the fixed step too.** Under `--oracle` the script's `sleep`
steps and the timeouts of its waits (`src/app/input/`) are timed by the client's clock, which is
then the fixed-step clock: `ScriptedWindow` gives the player `Ticks()` once per pump turn, so
`sleep 50000` is 50 s of the game's clock (3,000 frames at the default step, fewer when the game
sleeps or spins), however long the host takes. `oracle_input.exe`, the Wine driver, is a process of its own and times its steps by the
wall clock (`GetTickCount` is not replaced in it). So when a native trace is compared with a Wine
trace, a step's time is a delivery order, not a time: each run plays the same steps in the same
order, and a click reaches the game at the frame where its screen is up, but not at the same frame
in both. The traces agree only from samples the clicks no longer reach: the oracle mission's
samples begin after `PLAY GAME`, and the script fixes ships, map and positions, so a frame or a
hundred more spent on a menu does not change them. A step file whose clicks land during the
mission would make the two traces part at that click.

**The default step is 16.667 ms** (1000/60, to the microsecond, the unit `--fixed-step` takes).
The original's typical frame on a recording machine is not known (no original trace exists yet,
DESIGN §12.9), so the step is the brief's fallback, 60 frames a second, which is also the rate the
native menu settles at on the host clock. Whatever the step, the original under
Wine is recorded with the same one (`oracle/driver/README.md`, "The fixed step under Wine").

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

### On the fixed step

The same machine and setup, the lifted build of the fixed-step branch, `--fixed-step` at its
default (16.667 ms), headless, `--record-trace`, one run at a time. With the clock pinned a run is
also much shorter: the mission's 640 samples take 43,650 frames (about 728 s on the game's clock)
and 4 to 5 minutes of real time headless (13 windowed, where frames are presented), against 13 headless before.

| Run | Mode | END | Exit | Tick lines (SHA-256 of the 641 `tick<TAB>hash` lines) |
| --- | --- | --- | --- | --- |
| `h1` | `--headless --record-trace` | 220 s | 0 | `18fd7cefaa2c4d602d7d111988849899e4374d157d9cbb72070eee5970078a46` |
| `h2` | `--headless --oracle h1 --record-trace` | 279 s | 4 (tick 3) | `fd455f8a719da2f4ab4a10adc12ef1eaa02a3a962ba35ba5a7f062250e91ccff` |
| `w1` | `--windowed --oracle h1 --record-trace` | 756 s | 4 (tick 3) | `cdba757473d5847f246a12c2aa7dd98b6fefd8f0c3e476fedc084cc93936ad9d` |

Five more headless runs of the same binary gave the same two tick sequences and no third:
`18fd7cef...` twice and `fd455f8a...` three times. A run of an earlier build, whose busy-wait
guard took a million reads instead of 10,000 (65 minutes to `END`), gave `fd455f8a...` too, so the
guard's count does not choose between them. Every run on the fixed step agrees with every
other up to sample 2; the two sequences part at sample 3, in the floats (the distance is
`318.271606` in one and `318.271637` in the other), by a hair where the runs on the wall clock
parted at sample 1 by far more. The windowed run `w1` follows `h2`'s sequence through sample 497 and
parts from it at sample 498 in one field, the damage of `c101` (`5.83333397` against `0`; every
position the same). These runs are of the build before the timers moved onto frames.

The main thread's clock is the same in both: a diagnostic build that logged every 50th frame's
reading, every sleep and every spin step gave the same lines, line for line, up to `END`, in two
runs that gave the two different sequences. The game starts a periodic multimedia timer of 25 ms
(`timeSetEvent`, procedure `0x00637BA0`) at boot, whose callback ran on its own thread by the
kernel's real clock and read the clock about once per frame in the mission, so it was the first
suspect. **The experiment does not confirm it.** With the timers on frames (above), two headless
runs and a windowed one, the build of `9d72611` plus the timers:

| Run | Mode | Exit | Callbacks on frames | Tick lines |
| --- | --- | --- | --- | --- |
| `h1` | `--headless --record-trace` | 0 | (not printed by that build) | `18fd7cefaa2c4d602d7d111988849899e4374d157d9cbb72070eee5970078a46` |
| `h2` | `--headless --oracle h1` | 4 (tick 3) | (not printed) | `fd455f8a719da2f4ab4a10adc12ef1eaa02a3a962ba35ba5a7f062250e91ccff` |
| `w1` | `--windowed --oracle h1` | 4 (tick 3) | (not printed) | `874300c7164927b7123d878a65d5a203993c69a7acf8be2a3f6498c616e93d35` |
| `h3` | `--headless --record-trace` | 0 | 30,689 (767,236 ms / 25) | `fd455f8a719da2f4ab4a10adc12ef1eaa02a3a962ba35ba5a7f062250e91ccff` |

The headless runs give the same two sequences as before the change, parting at sample 3 in the
same field (`dist` of `c101` and `c201`, `318.271637` in `h1` against `318.271606` in `h2`, every
grid position the same), and `w1` follows `h2` to sample 25 and parts at 26 in `dist` again
(`315.039459` against `315.039642`). The timer's callbacks now land on the same frames in every
run, so they are not what parts them; the change stays, as one source fewer. The cause is
unconfirmed. The next candidates are the C runtime's `srand` sites seeded from the host
(`oracle/README.md`, "Determinism assumptions" 1): from `time(NULL)` (`0x005098C9`, `0x006C2267`,
`0x006DE5BB`), which under `--oracle` is still the host's time of day, and from
`GetCurrentThreadId()` (`0x0073325F`); a seed taken at a different second would move the ships
within their grid cells, which is what `dist` shows with every grid position equal.

The captures of each screen, taken by window id with `screencapture -l`, are in the pull request.
