# spike/delink: P1-3, delinking StarFleetOP.exe and relinking it

`StarFleetOP.exe` 2.5.5.2 is cut back into one relocatable COFF object with
ghidra-delinker-extension, relinked with the VC6 SP4 `link.exe` at another base, checked byte for
byte, booted under Wine, and relinked again with one function replaced by code compiled from the
SDK source. Findings and numbers are in `REPORT.md`.

## Layout

| Path | What |
|---|---|
| `run_all.sh` | The one command: everything below in order, then `results/` refreshed (about 25 min; `BUILD=0` and `BOOTS=0` skip those parts, `RUNS` sets boots per exe (default 3); fails if a run that stayed up has a registry trace unlike the original's) |
| `env.sh` | Shared settings: paths, Ghidra limits (8 GB heap, 4 cores), `run_headless` with timing |
| `delink.sh` | `setup` (extension), `import` (fresh copy of the shared `.gzf`), `prep`, `export` |
| `relink.sh` | Import libraries, resource object, `link.exe`; `BASE` picks the image base |
| `boot.sh` | Boots an exe in the spike's own Wine prefix (no CD key), lists Wine's windows, screenshots; `BOOT_WINEDEBUG` sets `WINEDEBUG` (`fixme-all,+reg` for a registry trace) |
| `replace/build.sh` | Compiles `tOSVersion::IsWindows98` from the SDK source and extracts its section |
| `overrides.tsv` | Per-operand keep/drop decisions the prep rules get wrong (none needed so far) |
| `symbols.tsv` | Names given to addresses so compiled code can link against the object |
| `ghidra_scripts/SfcDelinkPrep.java` | Prepares the program (see its header and `REPORT.md`, "What prep fixes") |
| `ghidra_scripts/SfcDelinkExport.java` | Synthesizes relocations and exports the object, with full logs |
| `ghidra_scripts/Sfc*Probe.java`, `SfcDelinkSurvey.java`, `SfcScalarRefs.java`, `SfcUnitDump.java` | Read-only diagnostics used while building prep (each says its arguments in its header; run with `run_headless` from `env.sh`) |
| `tools/imports.py` | Maps the 353 IAT slots to import-library symbols; `.def` files for DBGHELP and mss32 |
| `tools/rsrc2obj.py` | Carries the original `.rsrc` over as a COFF object (no `cvtres` under wibo) |
| `tools/validate.py` | Original vs relinked: every relocation and every other byte; `--exclude LO-HI=SYMBOL` for a replaced function |
| `tools/regtrace.py` | Compares the registry keys and values two `+reg` boots touched |
| `tools/compare_function.py` | Compares a replaced function with the original, byte for byte, explaining each difference by a moved data address |
| `tools/candidates.py`, `candidate_notes.tsv` | A verdict for every unrelocated value that looks like an image address; the notes hold the verdicts of reviews by hand |
| `tools/coffsyms.py`, `tools/extract_function.py` | Lists a COFF object; copies one function's section out of one |
| `tools/pecoff.py` | Small PE/COFF reader and COFF object writer |
| `tests/test_tools.py` | Unit tests for the Python tools (`python3 -m pytest spike/delink/tests`) |
| `results/` | The small outputs the report quotes: summaries, validator and link results, boot results, registry comparison |

Generated data lives outside the repo in `$SFCOP_ROOT/work/delink/`:
the extension download and settings, the Ghidra project copy (`projects/`), `out/` (object,
relocation table, import libraries, relinked exes and maps, validator output), `replace/` (staged
SDK copy and compiled objects), `wine/prefix` (the spike's own prefix, no CD key) and `boot/`.

## Rerun

Prerequisites: the shared Ghidra export `work/ghidra/StarFleetOP_2552.gzf` (task P0-3,
`ghidra/build.sh`), Homebrew Ghidra 12.2, Docker Desktop with the `sfcop-msvc6` image
(`toolchain/build-image.sh`), the VC6 `Lib/` set (`toolchain/vc6-lib/fetch.sh`) and the oracle's
Wine (`oracle/README.md`).

```sh
spike/delink/run_all.sh
```

The steps it runs, one at a time:

```sh
spike/delink/delink.sh                       # setup, import, prep, export: about 1 minute
spike/delink/relink.sh                       # -> work/delink/out/exe/sfcop_relinked.exe, about 20 s
BASE=0x400000 spike/delink/relink.sh "$W/out/exe/sfcop_samebase.exe"   # control build
python3 spike/delink/tools/validate.py <original exe> "$W/out/exe/sfcop_relinked.exe" \
    "$W/out/exe/sfcop_relinked.map" "$W/out/obj/sfcop.relocations.tsv" "$W/out/validate_relinked"
BOOT_WINEDEBUG=fixme-all,+reg spike/delink/boot.sh <original exe> reg-original 20 45 75
BOOT_WINEDEBUG=fixme-all,+reg spike/delink/boot.sh "$W/out/exe/sfcop_relinked.exe" reg-relinked 20 45 75
python3 spike/delink/tools/regtrace.py "$W/boot/reg-original/wine.log" "$W/boot/reg-relinked/wine.log" \
    reg-original reg-relinked

# One function replaced
spike/delink/replace/build.sh
OBJ="$W/out/obj/sfcop_noiswin98.obj" EXCLUDE_FN=006f55a0 spike/delink/delink.sh export
OBJ="$W/out/obj/sfcop_noiswin98.obj" spike/delink/relink.sh "$W/out/exe/sfcop_replaced.exe" \
    "$W/replace/IsWindows98_only.obj"
python3 spike/delink/tools/validate.py <original exe> "$W/out/exe/sfcop_replaced.exe" \
    "$W/out/exe/sfcop_replaced.map" "$W/out/obj/sfcop_noiswin98.relocations.tsv" \
    "$W/out/validate_replaced" --exclude '006f55a0-006f55bc=?IsWindows98@tOSVersion@@QAE_NXZ'
BOOT_WINEDEBUG=fixme-all,+reg spike/delink/boot.sh "$W/out/exe/sfcop_replaced.exe" reg-replaced 20 45 75
```

(`W=$SFCOP_ROOT/work/delink`.) `prep` changes the project copy, so
run `import` before every `prep`; `export` opens it read-only.
