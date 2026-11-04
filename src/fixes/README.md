# src/fixes: the fix switches

Every change to the original's behavior is a named switch with a default (DESIGN §5.14, §6.9).
`docs/FIXES.md` is the list; `switches.def` declares the same switches in the same order.

| File | What |
| --- | --- |
| `switches.def` | The switches: one `SFCOP_FIX_SWITCH` line each, and a block for each build switch. Not a contract file: adding a fix changes this, not `fixes.h` |
| `fixes.h` | The mechanism (a contract): `SFCOP_FIX(name)` for `#if` in `game/`; `sfcop::fixes::Settings`, `registry`, `install`, `isEnabled` |
| `Fixes.cpp` | The registry table read from `switches.def`, `settings/fixes.gf` and `--fix` parsing, the effective set |
| `FixesTest.cpp`, `FixesTestSwitches.def` | Defaults, the command line, the file, precedence, rejected names and values, `#if`, the `switches.def` form |
| `FixesVc6Test.cpp` | `fixes.h` as VC6 reads it: every build switch off, no C++ API |

## Two kinds of switch

- **runtime**: read in `src/` with `sfcop::fixes::isEnabled("fix_name")`. Its position is the
  default, then `settings/fixes.gf`, then `--fix fix_name=on|off`; the later source wins whatever
  order they are applied in. The command line parser hands its `--fix` values to
  `Settings::applyCommandLine`, the start-up code reads the file with `Settings::loadFile`, prints
  each `Problem`, and calls `install` before the game's threads start.
- **build**: read in `game/` with `#if SFCOP_FIX(fix_name)`, original code in `#else`. Its position
  is fixed when the file is compiled; the registry reports that position, and the file or the
  command line may name it only in that position.

`settings/fixes.gf` has the original's `.gf` form: `fix_name = on` (or `off`, `1`, `0`, optionally
quoted) one per line, `//` comments, no sections. The settings writer (B5) writes it; here it is
only read.

## Why the byte check runs with every fix off

The byte check compiles `game/` with VC6 and compares the object code with the original's bytes
(DESIGN §4.8, §7.2). A function with a fix compiled in is no longer the original, so it could never
match, and a fix would cost the function its `exact` status. So under VC6 `SFCOP_FIX(name)` is 0
for every switch whatever its default: the `#else` branch, the original code, is what the byte
check sees, and a fix never changes a function's `FUNCTION:` status (CONVENTIONS §4.1). The same
holds for the 32-bit build, which is the original relinked. The native build compiles the fixes in
at their defaults; `-DSFCOP_FIXES_OFF` builds it with every build switch off.

## Adding a fix

CONVENTIONS §8, "Adding a fix", is the checklist. For this directory:

1. Add the row to `docs/FIXES.md` first: name, default, origin, kind, behaviors, tests.
2. Declare the switch in `switches.def`, in the same order as the document:
   - runtime: one line, `SFCOP_FIX_SWITCH(fix_name, Runtime, 1|0, Project|Community, "...")`.
   - build: the line, `SFCOP_FIX_SWITCH(fix_name, Build, 1|0, Project|Community, "...")`, with its
     block just above it holding the same default:

     ```c
     #ifndef SFCOP_FIX_BUILD_fix_name
     #define SFCOP_FIX_BUILD_fix_name 1      /* 1 default on, 0 default off */
     #endif
     #define SFCOP_FIX_fix_name() SFCOP_FIX_POSITION(SFCOP_FIX_BUILD_fix_name)
     ```

     The registry reads `SFCOP_FIX(fix_name)` for a build line, so a line without its block does
     not compile, and a doctest checks that the block and the line agree.
3. Put the fix behind the switch: `isEnabled` in `src/`, or `#if SFCOP_FIX(fix_name)` around the
   smallest region in `game/` with the original in `#else`.
4. Test both positions: for a runtime switch, `Settings` with `--fix fix_name=on` and `=off`; for a
   build switch, the code compiled with `SFCOP_FIX_BUILD_fix_name` 1 and 0. The byte check still
   passes with the fix off.

A fix touches `switches.def`, `docs/FIXES.md` and the code it changes. `fixes.h` is a contract file
(`.github/hotspots.txt`) and does not change when a fix is added.
