# SFCOP-decomp — Fixes

Every change this project makes to the original's behavior is a named switch with a default, and
this is the list of them (DESIGN §5.14, §6.9). The switches are declared in
`src/fixes/switches.def`; this document and that file list the same names in the same order. The
mechanism, `src/fixes/fixes.h`, does not change when a fix is added. How to add one:
CONVENTIONS §8, "Adding a fix", and `src/fixes/README.md`.

**Defaults.** A fix for a crash or a hang defaults on. Anything a player could call a rules change
defaults off. The owner reviews each default as the list fills (DESIGN §12.10).

**Origin.** `project` for a fix this project found; `community` for a community patch, reimplemented
from its diff and never copied. A community row names no one.

**Kind.** Where the switch is read, which decides how it is changed:

- `runtime`: read in `src/` through `sfcop::fixes::isEnabled`. Its position comes from the default,
  then `settings/fixes.gf` in the per-user data directory (one `fix_name = on|off` per line), then
  `--fix fix_name=on|off` on the command line; each later source wins.
- `build`: selects an override unit, `src/fixes/overrides/<Dir>/<unit>.cpp`, a copy of the fixed
  function with the change made, annotated `// OVERRIDE: STARFLEETOP 0x... fix_name`. On, the
  native build links the override's object in place of the original's; off, the original. `game/`
  is never edited for a fix (DESIGN §5.14, §6.9). The position is fixed when the build is
  configured: the default, or the value of `-DSFCOP_FIX_BUILD_fix_name=0|1`, and off for every
  switch under `-DSFCOP_FIXES_OFF`. The byte check and the 32-bit build never compile an override.
  The settings file and the command line may name it only in the position it was built in.

## Columns

| Column | Content |
| --- | --- |
| Name | The switch, `fix_` and a lower_snake_case verb phrase (CONVENTIONS §3) |
| Default | `on` or `off` |
| Origin | `project` or `community` |
| Kind | `runtime` or `build` |
| Behaviors | The DESIGN §5 behaviors it changes (§9 where it closes a hole in untrusted input), and what the fix does in one clause |
| Tests | The tests that hold each position |
| Pull request | Where the fix and its switch landed |

## The fixes

| Name | Default | Origin | Kind | Behaviors | Tests | Pull request |
| --- | --- | --- | --- | --- | --- | --- |
| `fix_hle_named_objects` | on | project | runtime | §5.2: a kernel object the game creates with an empty name during boot is its own object, not one shared by every such object | `spike/lifter/hle/tests/hle_selftest.c` (`test_unnamed_events`); `src/fixes/FixesTest.cpp` ("the installed set answers isEnabled for the empty-name object fix in both positions") | #8 (the fix); #19 (the switch) |
| `fix_example_rules_change` | off | community | build | §5.4: an example of a rules change | none | example, no code |

### `fix_hle_named_objects`

The game creates its worker events, and some mutexes and semaphores, with an empty name rather
than none. On Windows an empty name is no name, so each is its own object. An HLE that treats the
empty string as a name makes them all one shared object: a worker thread wakes on another thread's
event, finds no job, and reads through a null job pointer, in some runs and not others. On, an
empty name is no name, as on Windows. Off reproduces the shared object, for diagnosis only.

The fix is in the HLE kernel objects, below the Win32 surface; the game's code is unchanged. Until
the Wave 1 kernel32 module reads this switch, the lifter runtime applies the fix in every position
(`cmake/lifter/patches/pcrecomp-kernel32-unnamed-objects.patch`).

### `fix_example_rules_change`

Not a fix: this row shows the form of a rules change, which defaults off, and is declared nowhere
in code. It is removed when the first real rules change is listed.
