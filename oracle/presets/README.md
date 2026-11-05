# The oracle on every preset (A4)

The replay oracle runs headless on the lifted client of each preset's build, and the traces are
compared pair by pair (DESIGN §5.10: one trace, no tolerance). The three presets are
`macos-arm64` (the client runs natively), `linux-x64` (in the pinned `linux/amd64` container) and
`windows-x64` (under Wine); how each runs is `cmake/toolchains/README.md`.

| File | What |
| --- | --- |
| `run_preset.sh PRESET` | Runs the client on that preset's build and writes `ROOT/PRESET/MISSION/INPUT.trace` with the build line `native PRESET COMMIT`. Exit 0 written, 1 the client failed or left no trace, 2 bad arguments, 3 skipped (the reason on standard output, `skip: ...`) |
| `marks_trace.py` | Makes a trace from the client's `[mark]` lines (the menu path) |
| `compare_presets.py ROOT PRESET...` | Compares every pair, prints the SHA-256 of each trace's tick lines, and with `--expected DIR` compares each preset with the recorded traces (`compare.py --tree`). Exit 0 identical, 4 a tick differs (the first is printed), 2 a trace missing, truncated or malformed or a compared header field differs |
| `ci.py` | What the workflow decides, as a tested script: `gate` (which changed paths the oracle judges), `run` (each preset in turn; exit 0 ran, 3 skipped with a `::warning::` naming it and why, else the step fails after the others ran), `compare` (with `--expected`; warns when fewer than two presets ran) |
| `../../tests/test_oracle_presets.py` | Shell and comparison tests with a stand-in client and the trace fixtures |

## Two kinds of run

- **MenuPath** (input `menu`, the steps file `oracle/driver/menu.steps`): the client boots past the
  key prompt to the main menu and clicks SINGLE PLAYER. The trace has one tick per mark the game
  reached, in order (`Select Game`, `Setup`, ...), hashed as `mark 0x<ADDRESS> <name>`, and a last
  tick `end`. Times are not hashed (they differ between runs); they are written beside it in
  `menu.timings`. It needs no mission module, so it is what the presets are compared on until the
  skirmish plays.
- **OracleSkirmish** (input `skirmish`): the oracle mission plays and the trace is made from its log
  with `oracle/trace/mklog.py`, as `oracle/trace/FORMAT.md` describes. The client writes the log
  under `--logs DIR` where it has that flag, else under `logs/` of its data folder; only
  `OracleTrace.txt` is read.

## The data folder

The oracle needs the menu, so it needs a stored key. CI has none of its own. The runner's prepared
data folder is named by the environment variable **`SFCOP_ORACLE_DATA`** (the owner sets it on the
runner; the same name `oracle/driver/run_native.sh` reads). The runner script copies the folder,
whole, to a scratch folder under the build folder, runs the client on the copy and removes it at
the end, so the original is never written to and nothing from it is kept. The path is never printed;
unset, the step skips with a notice, and there is no fallback to a per-user folder.

## The skirmish: what the client and the runner must provide

- **`--logs DIR`.** The runner greps the client's `--help` for the string `--logs` and passes
  `--logs <scratch>/logs`; the mission's log is then `<scratch>/logs/OracleTrace.txt`, the only file
  read. A client without that flag makes the skirmish skip (exit 3, "no --logs flag yet").
- **The mission module, per host ABI** (`src/app/scripts/ModuleContract.md` §2). The runner gives the
  client `SFCOP_MISSION_MODULES=<SFCOP_ORACLE_MODULES>/<preset>` (or `--modules DIR`), and
  `<that>/OracleSkirmish/module.json` must exist. Each preset's modules are translated with
  `tools/scr2native/scr2native.py` on that preset's toolchain; a preset without its module skips the
  skirmish with that reason and never loads another ABI's module. For the Linux container the
  modules root is mounted read-only (`SFCOP_RUN_MOUNTS`) and set inside by the wrapper.
- **Timeouts.** The skirmish's client watchdog defaults to 900 s; its workflow step allows 20 minutes
  per preset (60 for three) and the job 90. Both come down to what A2 measures.

## Niceness

`nice -n 19` runs the client on macOS; under Wine the host's wine processes inherit it. For Linux,
`nice -n 19 docker ...` would nice only the docker client, so `run_preset.sh` writes
`$BUILD/nice-exec.sh` (`exec nice -n 19 "$@"`) into the build folder, which the container mounts, and
runs the client through it inside the container. Docker on a Mac runs the container in a VM: inside
it, the niceness orders only the VM's own processes, not the Mac's.

## The client's error output

When the client fails, the last five lines of its standard error go to the CI log, minus any line
that names the prepared data folder or its scratch copy. Nothing else of it is kept, and the
scratch copy is removed at the end of a run and again at the start of the next (a killed run).

## Cost

Measured on the laptop with the menu path (a client run lasts until its 60 s watchdog; boot to the
Setup mark takes 16 to 23 s): macOS 63 s, Linux (container start included) 62 s, Windows skipped,
the data copy under 1 s (1 MB), about 2.3 minutes for the job with checkout and upload. The job on
pull request 51's first run took 8 s: it skipped because the runner had no `SFCOP_ORACLE_DATA`.
The skirmish adds up to 20 minutes per preset by its timeout; A2's measurement replaces that.

## CI

The `oracle` job of `.github/workflows/pull-request.yml` runs after the builds, for a pull request
that touches `game/`, `src/`, `oracle/presets/`, `oracle/driver/`, `oracle/trace/` or that workflow
(this pull request does, through the last). It runs the three presets one after another at
`nice -n 19`, compares the traces of those that ran, and keeps the `.trace` files for 7 days. A
preset with no lifted client skips with its reason: `windows-x64` builds `src/` and the tests only
until the lift's runtime builds there, and every preset skips while the runner's build is made
without the lift.

The runner's variables: `SFCOP_ORACLE_DATA` (this), `SFCOP_ORACLE_MODULES` (the skirmish's modules
root) and, for the lifted builds, `SFCOP_INSTALL_DIR` and `SFCOP_PCRECOMP_DIR`
(`.github/workflows/pull-request.yml`, the `build` job). A preset that skips emits a `::warning::`
naming it and why, and so does a run where fewer than two presets ran, so a green job with one
preset is never silent.
