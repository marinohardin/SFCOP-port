# Task board

Status values: `todo`, `active`, `blocked`, `done`. Each task owns the directory in its row.
Sessions update only their own status line. Integrator (Marino's main session) commits.

## Phase 0: foundations (Nov 2026)

| ID | Task | Owns | Depends on | Done when | Status |
|---|---|---|---|---|---|
| P0-1 | Repo scaffold, rules, hashes | root files, `analysis/` | - | `CLAUDE.md`, `TASKS.md`, `.gitignore`, hashes committed | done |
| P0-2 | **VC6 SP4 toolchain container.** amd64 Docker image with decomp.me `msvc6.4` run under wibo (built from `main` so `link.exe` works; Wine fallback). Calibration: compile the 32 `ScriptInterface/*.cpp` with the `.dsp` flags, objdiff each against the member in `ScriptInterfaceR.lib`, report the byte-exact rate and every flag that had to change. | `toolchain/` | - | `toolchain/README.md` reproduces image + calibration; `toolchain/calibration/report.md` with per-file match % | todo |
| P0-3 | **Ghidra project and symbol recovery.** Headless Ghidra (brew install, 12.x) import of `StarFleetOP.exe` into a project under `work/ghidra/`; RTTI analyzer + class recovery; FID database built from the five SDK libs and applied; apply `analysis/results/StarFleetOP_2552_sdklib_matches.tsv`; map functions to original `.cpp` via assertion strings; export `ghidra/exports/functions.csv` (address, size, name, source, library flag), `classes.csv`, `vtables.csv`. | `ghidra/` | - | exports present; README reproduces headlessly; counts reported | todo |
| P0-4 | **Lifter spike (Route L).** Clone and build `sp00nznet/pcrecomp` (and survey alternatives) under `work/lifter/`; lift `StarFleetOP.exe`; measure: functions lifted vs total, how MSVC C++ EH frames / `__thiscall` / RTTI are handled, which Win32 imports its HLE covers vs the game's import list, what DDraw/D3D7 surface the game needs. Try to get the lifted output to compile for arm64 macOS. Report, do not polish. | `spike/lifter/` | - | `spike/lifter/REPORT.md` with measurements and a go/no-go recommendation; lifted output in `work/lifter/` | todo |
| P0-5 | **Replay oracle and Mac baseline.** Install Gcenx Wine (or CrossOver if present) with the untouched 2.5.5.2 game under `work/wine/`; confirm it boots; design and write a mission script (`.scr`, source in `oracle/scr/`) that logs per-tick ship state via the scripting API, plus a driver that runs a fixed skirmish and hashes the log. Compile the `.scr` once P0-2's container exists (coordinate via `TASKS.md`); until then, write and dry-run everything else. | `oracle/` | P0-2 for compiling | boot confirmed on Mac; oracle source + driver written; first checksum trace recorded if compiler available | todo |
| P0-6 | **Third-party source inventory.** Pin Crypto++ version from `cryptlibR.lib` class inventory vs `History.txt`; fetch Homeworld `src/Titan` headers; evaluate `madebr/WON` WONAPI SDK (provenance, licence); locate GameSpy SDK sources; MSVC 6 SP4 CRT/STL header availability; Miles `mss.h`. Vendor what is clearly usable into `third_party/<name>/` with a `PROVENANCE.md` each; list the rest with reasons. | `third_party/` | - | `third_party/INVENTORY.md`; vendored trees with provenance | todo |
| P0-7 | **Fuzzy lib matching.** Extend `analysis/scripts/libmatch.py` into a relocation-aware, instruction-level matcher (mask operands, hash mnemonic sequences; Capstone) to recover the 6,728 `nomatch` and 3,412 `small` SDK functions in the 2.5.5.2 exe; resolve `multi` matches via call-graph context. Output a second TSV with confidence scores. | `analysis/scripts/`, `analysis/results/` | - | `analysis/results/StarFleetOP_2552_sdklib_fuzzy.tsv`; counts before/after in `analysis/README.md` | todo |

## Phase 1: route decision (after P0-2, P0-3, P0-4)

| ID | Task | Owns | Depends on | Done when | Status |
|---|---|---|---|---|---|
| P1-1 | D3D7/DDraw HLE prototype against dmp3d entry points | `spike/d3d7hle/` | P0-3, P0-4 | main menu renders natively or documented blocker | todo |
| P1-2 | Delink proof (Route D fallback): ghidra-delinker, relink at new base, boot | `spike/delink/` | P0-2, P0-3 | relinked exe boots under Wine and passes oracle | todo |
| P1-3 | Route decision memo | root | P1-1, P1-2 | `ROUTE.md` | todo |
| P1-4 | Harness: build-and-verify script, hooks, queue, headless loop | `harness/` | P0-2, P0-5 | one function round-trips through the loop | todo |
