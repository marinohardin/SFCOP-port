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
names that tree, `SFCOP_SOURCE_DIR`), run with `python3` through the shell; `SFCOP_ORACLE_TOOLS`
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
- The time of day (`GetLocalTime`, `GetSystemTime`, `GetSystemTimeAsFileTime`, and the C
  runtime's `time()`, which reads them) is pinned too: 2026-01-01 00:00:00 UTC
  (`FixedStepClock::kEpochUnixSeconds`) plus the game's clock, the same in both zones, so the zone
  offset is 0 on every host. Reading it does not count towards the spin guard below. Timed waits
  (`WaitForSingleObject` with a timeout) stay on the kernel's real clock.
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
run, so they are not what parts them; the change stays, as one source fewer. The cause is in
"Run to run", below.

The captures of each screen, taken by window id with `screencapture -l`, are in the pull request.

### Run to run

**The cause: the oracle mission's own `srand(time(NULL))`.** The mission module has a C runtime of
its own, and the SDK's `tScriptInterface::mInitialize` seeds it from `time(NULL)` when the mission
starts: in a trace of every time-of-day read (the `[time]` lines `--trace` now prints), the run's
last is `GetLocalTime` and `GetSystemTime` (which the C runtime's `time()` reads) from `sub_6602D00D`, in the translated mission at base
`0x66000000`, at frame 3,964, on the main thread. The seed pin (`oracle/README.md`, "Determinism
assumptions" 1) reaches the game's two generators, not the script's, and the time of day was the
host's, so a run started in another second drew other numbers from the script's `rand()`. The
two traces part at sample 3 in `dist` alone (`318.271637` against `318.271606`, one unit in the
last place of a float, every grid cell and speed equal), and the gap grows sample by sample
(5 units at sample 5, 16 at sample 8): the same motion from a slightly different start.

The fixed-step clock now pins the time of day ("The fixed step", above). The other candidates:

| Candidate | Under the fixed step | Contributes |
| --- | --- | --- |
| `time(NULL)` in the mission module (`sub_6602D00D`, frame 3,964) | was the host's; now pinned | **yes**: two sequences before, one after |
| `time(NULL)` in the game: `srand` at `0x005098C9`, `0x006C2267`, `0x006DE5BB` | read at frames 0 and 302 (`sub_007390FA`), before the mission; now pinned | no: the seed pin at mission start replaces the main thread's seed |
| `GetCurrentThreadId()` (`srand` at `0x0073325F`, for a name printed as `SHI%lX`) | ids are handed out as threads are created (`[thread]` under `--trace`), all five by the main thread in the same order: 264, 268, 272, 276 at frame 0, 280 at frame 2 | no: the same ids in every run |
| The two threads that wait by `Sleep(20)` on the host's clock (thread 276, the sound part's update at `0x0066F040`; thread 268, a C runtime thread) | their loop counts follow the wall clock (thread 276 slept 9,090 times in one pinned run and 12,591 in another) | no: with the time pinned, the game's `.data` (0x838000, 93 pages) hashed every frame agrees in every run except four words of a C runtime `FILE` record at `0x0088EE88` (a console stream's buffer pointer, count and size) at a frame or two, which the simulation never reads |
| Timed waits on the kernel's real clock (four waits of 60 s by the C runtime's threads) | the same outcome in every run | no |

Before and after, on the same build except the pin, headless, `--record-trace`, the steps file
`oracle/driver/skirmish.steps`, one run at a time on a MacBook Pro (M1 Pro):

| Run | Time of day | Run time | Tick lines (SHA-256 of the 641 `tick<TAB>hash` lines) |
| --- | --- | --- | --- |
| `b1` | the host's | 165 s | `cdba757473d5847f246a12c2aa7dd98b6fefd8f0c3e476fedc084cc93936ad9d` |
| `b2` | the host's | 250 s | `cdba757473d5847f246a12c2aa7dd98b6fefd8f0c3e476fedc084cc93936ad9d` |
| `b3` | the host's | 151 s | `312196fd7ea89ecf8d6c04401f1a193f02643cfbc3fd6fd3eb6f1197b3add02a` |
| `b4` | the host's | 211 s | `312196fd7ea89ecf8d6c04401f1a193f02643cfbc3fd6fd3eb6f1197b3add02a` |

(The sequences differ from the ones above because #67 changed the headless screen, through which
the script's clicks reach the game.) Five consecutive runs of this branch, headless,
`--record-trace`, then windowed `--oracle` runs against `p1`'s trace:

| Run | Mode | Started | Run time | Exit | Tick lines |
| --- | --- | --- | --- | --- | --- |
| `p1` | `--headless --record-trace` | 14:11:48 | 247 s | 0 | `312196fd7ea89ecf8d6c04401f1a193f02643cfbc3fd6fd3eb6f1197b3add02a` |
| `p2` | `--headless --record-trace` | 14:15:55 | 291 s | 0 | `312196fd7ea89ecf8d6c04401f1a193f02643cfbc3fd6fd3eb6f1197b3add02a` |
| `p3` | `--headless --record-trace` | 14:20:46 | 353 s | 0 | `312196fd7ea89ecf8d6c04401f1a193f02643cfbc3fd6fd3eb6f1197b3add02a` |
| `p4` | `--headless --record-trace` | 14:26:39 | 301 s | 0 | `312196fd7ea89ecf8d6c04401f1a193f02643cfbc3fd6fd3eb6f1197b3add02a` |
| `p5` | `--headless --record-trace` | 14:31:40 | 262 s | 0 | `312196fd7ea89ecf8d6c04401f1a193f02643cfbc3fd6fd3eb6f1197b3add02a` |
| `w1` | `--windowed --oracle p1 --record-trace` | 14:36:02 | 1471 s | 4 (tick 3) | `27f4a1757a564ea068752f3f63151a1aa80f76f9ce6fdaec57290f7a559ccaf6` |
| `w2` | `--windowed --silent --oracle p1 --record-trace` | 15:01:39 | 672 s | 4 (tick 3) | `33241d59aca6e8d9f26c7cb6c3d0c2852ea6e425d4678f57a5910d9dc507ebf1` |

**Headless runs now agree; a windowed run does not yet.** Both windowed runs part from `p1` at
sample 3 with the same tick hash, in `dist` alone (`318.271606` against `318.271637`, the same pair
as before the pin), and from each other later; `--silent` (the audio stub) changes nothing, so the
difference comes in through the window or the Gpu, not the sound. A third windowed run and a
headless run of the same build, the game's `.data` hashed on every frame of the fixed-step clock,
first part in two words, at frame 3,965: `0x0083ECA4`, the game-clock stamp (in microseconds) that
`sub_00431708` keeps for a task it runs every 50 ms (it stamps it the first time it runs, when the
stamp is still negative), and `0x0085366C`, a float of seconds. Windowed, both are 16.667 ms, one
frame, ahead of headless from then on, so the 50 ms task falls on other frames. Four other pages of `.data` (at `0x00842000`, `0x0084B000`, `0x0088E000`,
`0x0088F000`) differ from the first frames on and were not identified. The windowed run counts each script step one
pump turn later than headless (SINGLE PLAYER at turn 3,002 against 3,001) at the same time on the
game's clock: one extra turn that read a window event and is not a frame. Where the window moves the
first call of `sub_00431708` by a frame is not found yet; it is outside the clock (the pump's turns,
`src/app/guest/`, and the scripted window, `src/app/input/`).

The trace files themselves differ only in their header (the build's commit and the run's own
lines), which `compare.py` does not hash. The script's steps fall on the frames they fell on
before the pin (2026-11-08, on develop `f8f63b0` and this branch alike): the window and the
first sleep at frame 1, SINGLE PLAYER at 3,001, SKIRMISH at 3,484, the mission's row at 3,967,
PLAY GAME at 4,140, the wait for the log at 4,142.
