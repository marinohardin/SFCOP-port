# The campaign mission that never started (X6)

**Finding.** A new single-player campaign stopped at the first mission's ACCEPT because the mission
modules ran out of thread-local storage indexes. Each translated mission module carries its own
Visual C++ 6 C runtime, which calls `TlsAlloc` when the module attaches (`DLL_PROCESS_ATTACH`) and
`TlsFree` when it detaches. `TlsAlloc` was a row of the kernel32rt module, on the `Threads`
component's slots; `TlsFree` was not (the game itself never imports it, so it has no row in
`src/hle/surface.tsv`), and a module's import of it was answered by pcrecomp's own KERNEL32 table,
which clears a flag in a table of its own and never gives the slot back to `Threads`. Every load
of a module therefore kept one of `Threads`' 128 slots for the rest of the run. The game loads all
89 modules at boot (`ScriptEntry`), and again when a campaign starts (`MissionSpecEntry`); the
127th module load took the last slot (the game's own C runtime holds one), and from then on every
module's C runtime found `TlsAlloc` answering `TLS_OUT_OF_INDEXES`, so its `DllMain` returned
FALSE and the loader refused it. At ACCEPT the game loads the chosen mission (`Met_10Patrol`),
which was refused like the others; the game does not check, reads guest address 0x4 in
`sub_00623760` and goes down.

The fix is a `TlsFree` row in kernel32rt (`ThreadLocal::Free` on `Threads::FreeTlsSlot`). With it,
both passes load all 89 modules, ACCEPT loads `Met_10Patrol` and opens the Mission Briefing, and
READY starts the mission: the tactical screen comes up and the mission clock runs.

Which of the brief's candidates it was: none of the four as written. The campaign table and the
save were read and written (the Campaign Selector lists the campaign's Auto Save; New Campaign
lists the campaigns), the mission the table picked (`Met_10Patrol`) is among the 89 translated
modules, the clicks reached the game, and no session was opened before ACCEPT (the game's
`fDirectPlayObject` lines come after READY, and the session opens). It was the loader refusing a
module that exists, for a reason in kernel32rt.

## The owner's path, reproduced

`oracle/driver/campaign.steps` plays it: Single Player, Campaign, New Game, Federation, BEGIN NEW
CAMPAIGN, the opening slides, the map scrolled back to the player's sector (see "Found on the
way"), one sector down, MISSIONS, ACCEPT on Patrol, READY.

```
sh work/X6/run.sh <sfcop> <label> oracle/driver/campaign.steps --headless 430   # or --windowed
python3 work/X6/judge.py <label>
```

(`work/X6/` is the task directory outside the repository; `run.sh` passes the test data folder by
path and `SFCOP_MISSION_MODULES`, `judge.py` reads the run's stderr.)

### Before: develop at 5e0d4b9 behaviour, headless

The build the owner ran (`work/X5/build`, develop 5e0d4b9 plus X5's quantum), headless, run
`before-h1`. The stop is the same as the owner's two windowed runs: 

- Last screen reached: the Mission Selector, Patrol offered; ACCEPT clicked at 219 s.
- The second pass: 38 modules `MissionSpecEntry` and load; from `Mul_12Slugfest` on, 51 refuse.
  89 + 38 = 127 module loads before the first refusal, plus the game's own index: 128, the
  number of slots `ThreadsHost` has.
- The last guest calls before the stall, from `--logs`/stderr:

```
[input] line 67 at 218960 ms, frame 14112: click 547 203
[scripts] Met_10Patrol: its DLL entry point refused to attach
[eh] access violation: read of guest 0x00000004 in sub_00623760 at 224.851 s
...
ICALL: unresolved VA 0x04040606 from 0x00713FB0
[eh] access violation: read of guest 0x0000001A in sub_00730740 at 225.364 s
[eh] unhandled guest exception 0xC0000005 raised at 0x00730740
```

  exit 1 at 226 s.
- The rows answered with an error: `TlsAlloc`, `TLS_OUT_OF_INDEXES` with
  `ERROR_NOT_ENOUGH_MEMORY`, once per refused module. A `--trace` run of the same path (headless,
  stopped at the Campaign Selector, `trace-x5`) counts the TLS rows of the boot pass: 90
  `TlsAlloc` (the game's one and one per module) and 89 `TlsFree`, so every module did free its
  index; the frees did not reach `Threads`. No `[scripts] TlsFree: answered by pcrecomp's
  win32hle` line is printed: pcrecomp's KERNEL32 table is registered by the host, so the
  module's import resolves to it directly.

### After: this branch

Windowed, nobody at the window, `--shots`: run `cal11` reaches the tactical screen, the mission
clock at 00:01:22; `silent-w1` the same with `--silent`. With the recentre step (before the
merge of #67, which changes only headless runs): `rc1` and `rc2` (map opened on its corner) and `rc3` (map opened on the player's sector)
all reach combat, and so do `final-w1` and `final-w2` on the merged tree (captures in the pull request). Headless: the table
below.

## Proof by repetition

This branch merged with develop at 4f390c2 (with #67, so READY can be clicked headless). Ten
headless runs of `campaign.steps`, one after another, each a new campaign on the same test data
folder, watchdog 430 s (`work/X6/final-htable.tsv`). "refused" counts modules whose DLL entry
refused to attach; "second pass" counts `MissionSpecEntry` calls after BEGIN NEW CAMPAIGN; "combat"
means the mission module loaded again after READY and the tactical screen wired up (the
`App could not find 'Arc1'` connection line that every mission start prints).

| run | exit | refused | second pass | ACCEPT | combat |
| --- | --- | --- | --- | --- | --- |
| final-h1 | 0 (watchdog, 431 s) | 0 | 89 | Met_10Patrol loaded | yes, READY at 288 s |
| final-h2 | 0 (430 s) | 0 | 89 | Met_10Patrol loaded | yes, READY at 288 s |
| final-h3 | 0 (431 s) | 0 | 89 | Met_10Patrol loaded | yes, READY at 288 s |
| final-h4 | 0 (430 s) | 0 | 89 | Met_10Patrol loaded | yes, READY at 288 s |
| final-h5 | 0 (430 s) | 0 | 89 | Met_10Patrol loaded | yes, READY at 288 s |
| final-h6 | 0 (430 s) | 0 | 89 | Met_10Patrol loaded | yes, READY at 288 s |
| final-h7 | 0 (431 s) | 0 | 89 | Met_10Patrol loaded | yes, READY at 291 s |
| final-h8 | 0 (430 s) | 0 | 89 | Met_10Patrol loaded | yes, READY at 292 s |
| final-h9 | 0 (430 s) | 0 | 89 | Met_10Patrol loaded | yes, READY at 288 s |
| final-h10 | 0 (430 s) | 0 | 89 | Met_10Patrol loaded | yes, READY at 288 s |

10 of 10 reach combat; no fault in any run. Before the recentre step and before #67, the same
batch reached the Mission Briefing in 8 of 10 (`after-h1` to `after-h10`): the two misses opened
the map on its corner, so the click one sector below the player's sector missed it.

## Found on the way, not fixed here

- **Headless, the cursor could not go below y = 479 or right of x = 639.** With no window,
  `hle/gdi32/GuestGdi.h` `screen()` kept its 640x480 default and `hle/user32/MessageQueue.cpp`
  clamped the cursor to it, although the game's display is 800x600, so READY at (598, 501) could
  not be clicked headless. Fixed apart, in its own pull request (#67: the headless screen follows
  DirectDraw's display mode); this branch has it from develop.
- **The campaign map does not always open on the player's sector.** In about a third of new
  campaigns (windowed or headless, same build, same steps) the map opens on its top-left corner
  instead; the stderr of the two kinds of run is the same line for line. `campaign.steps`
  therefore scrolls the map with the compass (one sector a click, stopping at the edge): seven
  north and twelve west reach the corner from either view, and six south and eleven east bring
  the player's sector back to the centre. Whether the original game does the same was not
  checked.
- **The game does not check a refused mission module.** `sub_00623760` reads through a null
  pointer when the mission it was given did not load (a game behaviour; reported, not changed).
- **The `tAtomicLock::mAcquire` count the owner saw comes from the refused modules.** The same
  windowed path to the campaign map and 70 s on it: 897,310,349 `mAcquire` against 69,986
  `mRelease` on the X5 build (`cal6`), 87,536 against 94,735 on this branch (`fix-cal6`); this
  branch into combat (`cal11`): 76,644 against 82,259. The faulting headless run on the X5 build:
  118,404,598 against 73,486. Which thread and caller spun while modules were refused was not
  recorded: with the fix the spin is not there to record.
- At teardown after the fault: `ICALL: unresolved VA 0x04040606 from 0x00713FB0` (the owner's
  run printed `0x006C0075 from 0x006F9420`), shutdown after the first fault, not a cause.
- `src/app/scripts/loader/GuestModules.cpp` says a mission's `TlsFree` is answered by pcrecomp's
  win32hle; with this fix kernel32rt answers it (the comment is out of date).
