# src/fixes: the fix switches

Every change to the original's behavior is a named switch with a default (DESIGN §5.14, §6.9).
`docs/FIXES.md` is the list; `switches.def` declares the same switches in the same order.

| File | What |
| --- | --- |
| `switches.def` | The switches: one `SFCOP_FIX_SWITCH` line each, and a block for each build switch. Not a contract file: adding a fix changes this, not `fixes.h` |
| `fixes.h` | The mechanism (a contract): `SFCOP_FIX(name)` for `#if` inside an override unit; `sfcop::fixes::Settings`, `registry`, `install`, `isEnabled` |
| `overrides/` | One unit per fixed game unit: copies of the fixed functions, `// OVERRIDE:` annotated, linked in place of the originals when their switch is on (DESIGN §6.9). Empty until the first fix to game code |
| `Fixes.cpp` | The registry table read from `switches.def`, `settings/fixes.gf` and `--fix` parsing, the effective set |
| `FixesTest.cpp`, `FixesTestSwitches.def` | Defaults, the command line, the file, precedence, rejected names and values, `#if`, the `switches.def` form |
| `FixesVc6Test.cpp` | `fixes.h` as VC6 reads it: every build switch off, no C++ API |

## Two kinds of switch

- **runtime**: read in `src/` with `sfcop::fixes::isEnabled("fix_name")`. Its position is the
  default, then `settings/fixes.gf`, then `--fix fix_name=on|off`; the later source wins whatever
  order they are applied in. The command line parser hands its `--fix` values to
  `Settings::applyCommandLine`, the start-up code reads the file with `Settings::loadFile`, prints
  each `Problem`, and calls `install` before the game's threads start.
- **build**: selects an override unit under `overrides/`, a copy of the fixed function with the
  change made; `game/` is never edited for a fix. Its position is fixed when the build is
  configured; the registry reports that position, and the file or the command line may name it
  only in that position.

`settings/fixes.gf` has the original's `.gf` form: `fix_name = on` (or `off`, `1`, `0`, optionally
quoted) one per line, `//` comments, no sections. The settings writer (B5) writes it; here it is
only read.

## Why the byte check runs with every fix off

The byte check compiles `game/` with VC6 and compares the object code with the original's bytes
(DESIGN §4.8, §7.2). A function with a fix compiled in is no longer the original, so it could never
match, and a fix would cost the function its `exact` status. So no fix is written into `game/`: a
fix to game code is a separate copy under `overrides/`, which the byte check and the 32-bit build
never compile, and a fix never changes a function's `FUNCTION:` status (CONVENTIONS §4.1). Under
VC6 `SFCOP_FIX(name)` is 0 for every switch whatever its default, so an override unit that uses
it inside compiles as the original there too. The native build links the overrides whose switches
are on at their defaults; `-DSFCOP_FIXES_OFF` builds it with every build switch off.

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
3. Put the fix behind the switch: `isEnabled` in `src/` for a runtime switch; for a build switch,
   copy the function from `game/` into `overrides/<Dir>/<unit>.cpp` with `// OVERRIDE: STARFLEETOP
   0x... fix_name` on the line before it, make the change, and comment the changed line. Never
   edit `game/` for a fix: `scripts/check.sh` fails on an `SFCOP_FIX` site there.
4. Test both positions: for a runtime switch, `Settings` with `--fix fix_name=on` and `=off`; for a
   build switch, the native build with `SFCOP_FIX_BUILD_fix_name` 1 and 0. The byte check is
   unchanged, since it never compiles an override.

A fix touches `switches.def`, `docs/FIXES.md` and the code it changes. `fixes.h` is a contract file
(`.github/hotspots.txt`) and does not change when a fix is added.
