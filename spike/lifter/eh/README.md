# spike/lifter/eh: P1-1, guest exceptions in the lifted build

Findings and coverage numbers are in `REPORT.md`. This page says what is here and how to rerun it.

## Layout

| Path | What |
|---|---|
| `scripts/run_eh.sh` | Reruns everything: setup, the guest test, the game lift, build and runs |
| `scripts/sandbox.sh` | The spike's sandbox, with `work/lifter-eh` at `/w` and this `scripts/` at `/s` |
| `scripts/eh_frames.py` | Static map of a VC6 image's exception frames: catching functions, prologue shapes, catch continuations, `__except` blocks, the CRT routines the lift replaces |
| `scripts/run_lift_eh.py` | The spike's lift driver plus landing pads from `eh_frames.py`, continuations made entries, the two CRT hooks, and `recomp_eh_table.c` |
| `runtime/sfcop_eh.c`, `.h` | Host side: `RaiseException` (dispatch over the guest's `fs:[0]` chain), `RtlUnwind`, `IsBadReadPtr`/`IsBadWritePtr`/`IsBadCodePtr`, the landing-pad frame list and the two transfers |
| `host/` | The spike host with its EH stubs replaced by the runtime, plus `--exe`, `--args`, `--no-osspecific` and `--fail-missing` |
| `guest/ehtest.cpp` | Guest-side tests, built with the VC6 SP4 compiler in `toolchain/` and lifted like the game |
| `tests/test_eh_frames.py` | Unit tests for `eh_frames.py` on hand-assembled bytes |
| `patches/pcrecomp-eh-landing-pads.patch` | 3-line change to pcrecomp's `generate.py` (see `patches/README.md`) |
| `results/` | Every measurement `REPORT.md` quotes |

Generated data lives outside the repo, in `$SFCOP_ROOT/work/lifter-eh/`:

| Path | What |
|---|---|
| `pcrecomp/`, `cat/`, `game/`, `gameroot/` | APFS clones of the spike's tree in `work/lifter/`, which is never written |
| `ehtest/` | The test program: source copy, `ehtest.exe` and its map, catalogue, lifted C, run logs |
| `out/gen` | The game's lifted C |
| `build-ehtest/sfcop`, `build-game/sfcop` | The two arm64 hosts |
| `runs/` | Game run logs, with the import trace |
| `venv/` | Our scripts' Python (capstone, pefile, pytest) |

## Rerun

Prerequisites: everything the spike's `README.md` lists (Docker Desktop with the `sfcop-lifter-py`
image, Xcode command line tools, Homebrew `cmake`, `ninja`, `pkg-config`, `sdl2`), the spike's
tree in `work/lifter/` (`spike/lifter/scripts/run_all.sh` makes it), and the `sfcop-msvc6` image
plus the VC6 libraries in `tools/vc6-lib/` (`toolchain/README.md`).

```bash
$SFCOP_ROOT/SFC2-decomp/spike/lifter/eh/scripts/run_eh.sh
```

That takes about 15 minutes, most of it the game build (6 minutes at `-j4`) and lift (2 minutes).
To rerun a subset, set `STEPS` to some of
`setup guest guest-cat guest-lift guest-build guest-test frames game-lift game-build game-run`:

```bash
STEPS="guest-test game-run" $SFCOP_ROOT/SFC2-decomp/spike/lifter/eh/scripts/run_eh.sh
```

`guest-test` fails the script unless all 14 cases pass and the uncaught throw is reported with
exit status 7. The unit tests:

```bash
$SFCOP_ROOT/work/lifter-eh/venv/bin/python -m pytest spike/lifter/eh/tests
```

All of this is local only, and none of it runs in `scripts/check.sh` or CI. The guest tests and
the game runs need what the prerequisites above list, on macOS arm64. The unit tests need the
lifter-eh venv for capstone and pefile.

`SFCOP_EH_TRACE=1` makes the runtime print one line per raise, unwind and transfer. The toolchain
wrappers find `tools/` from `SFCOP_ROOT`, which `run_eh.sh` sets; inside a worktree under
`.claude/worktrees/` their own guess points at the wrong folder.
