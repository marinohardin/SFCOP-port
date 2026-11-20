# SFCOP-port — Fixes

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
  native build links the override's object in place of the original's; off, the original. The
  decompilation (`decomp/op/`) is never edited for a fix (DESIGN §5.14, §6.9). The position is
  fixed when the build is configured: the default, or the value of `-DSFCOP_FIX_BUILD_fix_name=0|1`, and off for every
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
| Pull request | Where the fix and its switch landed: a bare number is a pull request in this repository, `SFCOP-decomp#N` one in the repository this was split from before 2026-11-16 |

## The fixes

| Name | Default | Origin | Kind | Behaviors | Tests | Pull request |
| --- | --- | --- | --- | --- | --- | --- |
| `fix_hle_named_objects` | on | project | runtime | §5.2: a kernel object the game creates with an empty name during boot is its own object, not one shared by every such object | the Phase 1 spike's self-test (`test_unnamed_events`, since removed); `src/fixes/FixesTest.cpp` ("the installed set answers isEnabled for the empty-name object fix in both positions") | SFCOP-decomp#8 (the fix); SFCOP-decomp#19 (the switch) |
| `fix_hle_low_memory_reads` | on | project | runtime | §5.9, §5.16: a read of guest 0x00010000-0x0001FFFF (a null pointer to an object over 64 KB, as in the Met_17Patrol and Met_24Anomaly victory states) gets environment-shaped text, as on the original's NT target, and the first read in each page is logged; off, the read ends the run. The oracle and CI run it off | `src/app/guest/LowMemoryTest.cpp` ("on: the first read of a page maps it read-only with the text, and the read resumes"; "off: the region is not armed and a read of it ends the run"; "off: the set-up reserves nothing and arms nothing"; "on: a write to the region still faults"); `src/fixes/FixesTest.cpp` ("the registry declares the low-memory read fix as a runtime project fix, default on"); `tests/test_oracle_presets.py`, `tests/test_run_native.py` (the oracle passes it off) | #13, #17 |
| `fix_smooth_scaled_frame` | on | project | runtime | §5.6: the game's frame, shown scaled into a window of another size, is filtered sharp-bilinear (nearest to the whole-number part of the scale, linear for the rest); off, every scale uses the nearest pixel | `src/hle/ddraw4/SurfaceTest.cpp` ("the frame is presented with the filter fix_smooth_scaled_frame names, sent once"); `src/platform/Gpu/GpuSdlTest.cpp` ("sharp-bilinear enlarges by the whole part of the scale before the linear step") | SFCOP-decomp#64, SFCOP-decomp#136 |
| `fix_integer_scaled_frame` | off | project | runtime | §5.6: the frame is shown at the largest whole-number scale that fits the window, with bars beyond; off, it fills the window at any scale | `src/hle/ddraw4/SurfaceTest.cpp` ("each present gives the window the whole-number scale fix_integer_scaled_frame names"); `src/platform/Gpu/GpuTest.cpp` ("the whole-number placement takes the largest whole scale that fits, centred") | SFCOP-decomp#136 |
| `fix_render_scale` | on | project | runtime | §5.6: Direct3D targets are rendered at the window's pixel density; off, at the game's own size | `src/hle/ddraw4/DirectDrawTest.cpp` ("the render scale is 1 with fix_render_scale off, at 8 bits, or with no window state"); `src/platform/Gpu/GpuSdlTest.cpp` ("a triangle drawn at render scale 3 is the same triangle rasterised 3 times larger") | SFCOP-decomp#146 |
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

### `fix_hle_low_memory_reads`

Some of the game's mission code reads through a null pointer to a large object, at an offset past
64 KB: the victory states of Met_17Patrol and Met_24Anomaly do. On the original's Windows NT
target that address held the process's environment block, so the read returned text and the
mission went on. On, the port reserves guest 0x00010000-0x0001FFFF without access; the first read
of each host page fills that page, read-only, with text shaped like an environment block, and the
read resumes. The read is recorded in the signal handler and logged later, outside it, as an
important line, at most once per page. A write to the region still ends the run. Off, nothing is
reserved and the read ends the run, as an unfixed port would.

The replay oracle and CI run it off (`scripts/check.sh`, `oracle/driver/run_native.sh`,
`oracle/presets/run_preset.sh`), so a recorded trace shows the original's code path and a new
read of the region fails the run instead of being covered over. The game's code is unchanged.

### `fix_smooth_scaled_frame`

The original drew at its display mode on a display of that size. The port shows the game's frame
in a window of any size, centred with its aspect kept, so the frame is scaled whenever the window
is not its size. On, the scaling is sharp-bilinear: the frame is enlarged by the nearest pixel to
the whole-number part of the scale, and the rest is linear, so pixels stay crisp with no uneven
widths, and at a whole-number scale it is exact. Off, every scale uses the nearest pixel: sharp, with uneven
pixel widths at sizes that are not whole multiples. At the frame's own size both positions show
its pixels as they are. The game's code is unchanged: `src/hle/ddraw4/` hands the position to the
Gpu (`Gpu::SetPresentFilter`).

### `fix_integer_scaled_frame`

On, the frame is shown at the largest whole-number scale that fits the window, centred, with bars
around it, so every pixel of the game is the same size on screen. Off, the frame fills the window
at any scale, its aspect kept. It defaults off because it changes how the game looks, not how it
runs. The same placement decides where mouse positions land and the render scale
`fix_render_scale` picks. The game's code is unchanged: `src/hle/ddraw4/` hands the placement to
the Gpu.

### `fix_render_scale`

The original rendered its Direct3D scenes at its display mode's size. On a high-density display
the port's window has more pixels than that, so a scene rendered at the game's size is enlarged
and blurs. On, Direct3D render targets are made at a whole-number scale: the frame's scale in
the window's pixels, rounded up, and limited by the Gpu's largest texture and the render scale
option. 8-bit modes, and a Gpu that refuses a scaled back buffer, stay at the game's own size.
Off, every scene renders at the game's own size. The game's code is unchanged:
`src/hle/ddraw4/DirectDraw.h` (`WantedRenderScale`) chooses the scale.

### `fix_example_rules_change`

Not a fix: this row shows the form of a rules change, which defaults off, and is declared nowhere
in code. It is removed when the first real rules change is listed.
