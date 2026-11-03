# spike/lifter: P0-4, lifting StarFleetOP.exe with pcrecomp (Route L)

Findings are in `REPORT.md`. How pcrecomp, redalert2-recomp and NFSU2Recomp work (EH, dispatch,
RTTI, x87, HLE) is in `MECHANISMS.md`.

## Layout

| Path | What |
|---|---|
| `scripts/run_all.sh` | Reruns everything: setup, RTTI, disassembly, lift, census, build, run |
| `scripts/sandbox.sh` | Runs a command in the `sfcop-lifter-py` container: no network, 4 CPUs, 6 GB, `work/lifter` at `/w` |
| `scripts/run_lift.py` | Lift driver (runs in the sandbox), adapted from redalert2-recomp's; writes `lift_stats.json` |
| `scripts/census.py` | Our static census: C++ EH tables, throw helper, `fs:` uses, DirectX GUIDs, COM call sites |
| `scripts/imports_vs_hle.py` | Game imports vs the shim names win32hle registers |
| `scripts/dx_inventory.py` | COM call sites into per-interface slot/method tables with win32hle status |
| `docker/Dockerfile` | The sandbox image (python:3.13-slim + capstone 5.0.7, pefile, unicorn) |
| `host/` | Spike host: `main.c`, `CMakeLists.txt`, and an SDL2_ttf stand-in header |
| `patches/pcrecomp-tib-hook.patch` | 2-line hook in win32hle's TIB setup so the host can install static TLS |
| `results/` | Every measurement the report quotes (logs, TSV, JSON, Markdown) |

Generated data lives outside the repo in `$SFCOP_ROOT/work/lifter/`:

| Path | What |
|---|---|
| `pcrecomp/`, `redalert2-recomp/`, `nfsu2-recompiled/`, `NFSU2Recomp/` | Clones (untrusted; read, not run on the host) |
| `game/StarFleetOP.exe` | Hash-checked copy of the exe |
| `gameroot/` | APFS clone of the install; the run's C: drive and the only place it writes |
| `cat/` | RTTI and disasm32 catalogues |
| `out/gen/` | The lifted C (140 files, 191 MB) |
| `build/sfcop` | The arm64 binary |
| `runs/` | Full run logs, with the import trace |
| `venv/` | Our scripts' Python |

## Rerun

Prerequisites:

- Docker Desktop running.
- Xcode command line tools.
- Homebrew `cmake`, `ninja`, `pkg-config` and `sdl2` (already present on this Mac). SDL2_ttf is
  not needed.

```bash
$SFCOP_ROOT/SFC2-decomp/spike/lifter/scripts/run_all.sh
```

That takes about 15 minutes in total:

- disassembly: about 4 minutes
- lift: about 2 minutes
- build: about 6 minutes at `-j4`

To rerun a subset, set `STEPS` to some of `setup rtti disasm lift census build run`:

```bash
STEPS="census run" $SFCOP_ROOT/SFC2-decomp/spike/lifter/scripts/run_all.sh
```

To run the host by hand:

```bash
cd $SFCOP_ROOT/work/lifter && ./build/sfcop --game gameroot --run --headless --trace --watchdog 60
```

The `setup` step pins pcrecomp to `ef3f0da` and applies the TIB patch. `run_lift.py --mark VA,...`
makes the given functions call `sfcop_mark()` on entry. The default is WinMain (0x0069A840), which
is how a run reports reaching it.

`results/run1.err.txt` and `results/run2.err.txt` come from the first two host versions: before the
OSSpecific shim, and before static TLS. `run_all.sh` reproduces run 3 only.
