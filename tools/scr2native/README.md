# scr2native

The mission script translator (DESIGN §5.9 path (b)): it takes a compiled `.scr` and produces a
native mission module that the native client loads in its place.

| What | Where |
| --- | --- |
| The module contract: entry points, version tag, import rule, files, refusals | `src/app/scripts/ModuleContract.md` (C8) |
| The loader interface and its mock | `src/app/scripts/ModuleLoader.h`, `ModuleLoaderMock.h` (C8) |
| The real loader: the files read on every host, mapped per platform | `src/app/scripts/loader/NativeModuleLoader.*`, `ModuleFiles.cpp`, `ModuleLoader_<platform>.cpp` |
| The provider LoadLibraryA asks for a `.scr` | `src/app/scripts/loader/GuestModules.*` |
| The guest image a module carries | `src/app/scripts/guest/GuestModuleAbi.h` |
| The import allow-list and its generator | `src/app/scripts/allowlist.txt`, `gen_allowlist.py` here |
| The host baseline list and its generator | `src/app/scripts/baseline-<host>.txt`, `gen_baseline.py` here |
| The translator | `scr2native.py`, `pe32.py` and `runtime/` here |

## What a `.scr` is

A `.scr` is a 32-bit Windows DLL built with VC6 from the SDK's templates. It links the scripting
library (`ScriptInterfaceR.lib`), the SDK libraries it depends on and the static C runtime
(`/MT`) into itself, exports `MissionSpecEntry` and `ScriptEntry`, and imports only Windows system
DLLs (KERNEL32 and a few functions of USER32 and ADVAPI32). The game reaches the mission only
through those two exports, then through the `tScriptEntry` virtual table and the callbacks the
game installs through it. `toolchain/build_scr.sh` builds one from source and
`toolchain/scr/check_scr.py` compares it with the shipped scripts.

All 88 shipped scripts are linked at 0x10000000, carry base relocations, have no TLS and fit in
0x7D000 bytes.

## Use

```sh
python3 tools/scr2native/scr2native.py <Name>.scr -o <modules root> [--base ADDR] [--keep-work]
python3 tools/scr2native/scr2native.py <Name>.scr -o <modules root> --check REFERENCE --trace ACTUAL
```

The module is written to `<modules root>/<Name>/` as `<Name>.dylib` (`.so`, `.dll`) and
`module.json`, replacing a module already there only once the new one is complete. The work
(the rebased DLL, the lift, the objects) goes to `--work`, by default
`$SFCOP_WORK/scr2native/modules/<Name>/`, and is removed once the module is installed unless
`--keep-work` is given; a lift is reused while its inputs (the rebased bytes, the pinned pcrecomp
revision, the patch set, the lifter's scripts, the seeds) are unchanged. It needs Docker for the
lift (the game's lifter image, `cmake/lifter/Dockerfile`), a pcrecomp clone (`--pcrecomp`, by
default `$SFCOP_PCRECOMP_DIR` or `$SFCOP_WORK/lifter-hle/pcrecomp`) and the host's C compiler. The
clone is only read: the lift and the compile use a copy with `cmake/lifter/patches/series`
applied, as the game's build does, kept in `<--work>/.pcrecomp/` under a name of its patch set
(`cmake/lifter/README.md`, "Copies outside a build folder").

Exit status: 0 written; 1 refused (a foreign import, a symbol off the lists, an entry point not
lifted); 2 the input or the command line is wrong; 3 a tool is missing or failed; 4 `--check`
found a difference, and no module was written.

The client looks for modules in `$SFCOP_MISSION_MODULES`, else in `mods/missions/` under its
working directory (the per-user data folder). It never reads the install for a module.

## The pipeline

```
1. read      the .scr is a PE32 DLL that exports MissionSpecEntry and ScriptEntry
2. check     every Windows import (DLL!Name) is on src/app/scripts/allowlist.txt; a foreign one
             stops the translation with the loader's refusal message, before anything is lifted
3. rebase    the image is relocated to the module's guest base (below)
4. lift      the game's lifter (pcrecomp, cmake/lifter/run_lift.py) on the rebased DLL, seeded with
             the exports, the entry point, the RTTI's virtual tables and every code address a
             relocation outside the code holds (virtual tables, callback and exception tables)
5. emit      module_image.c: the image's sections as data, its import slots by DLL!Name, the
             lifted functions by guest address, and the guest image descriptor
6. compile   the lifted C with the host compiler at -O2, with the game's lift definitions, against
             runtime/module_rt.h (the machine is the client's, bound by name at load) and
             runtime/module_glue.c (the binding, the version tag, the contract's exports);
             the lifted files also get runtime/module_rt_direct.h (below)
7. verify    every undefined symbol of the library is an allowed sfcop_import:DLL!Name or on the
             host baseline list; anything else refuses the module
8. --check   oracle/trace/compare.py on the two traces given; a difference writes no module
9. install   <modules root>/<Name>/<Name>.<dylib|so|dll> and module.json
             (name, source_sha256, translator_version)
```

### How lifted code reaches the client's machine

`module_rt.h` makes every runtime name (the registers, the x87 stack, the lifter's hooks) a pointer
the module holds, which `sfcop_bind` fills once from the client's resolver before any lifted code
runs. Lifted code stores to guest memory constantly, and with `-fno-strict-aliasing` any such store
might, as far as the compiler knows, have changed one of those pointers, so it would reload each one
after every store. `module_rt_direct.h`, forced into the lifted files only, declares the same
symbols again as `const` pointers under other C names (an `__asm__` label keeps the symbol) and
points the register names at those: each address is then loaded once per function and kept in a
register. The library's symbols, imports and exports are unchanged, and so is the module contract;
the shipped missions' code is about a fifth smaller.

This is sound because the glue alone defines and writes the pointers, only in `sfcop_bind`, before
the client first calls into the module; the lifted files never write them. The `const` in one
translation unit against the plain definition in another is a qualifier difference the separate
compile never sees together. Never build a module with link-time optimisation, which would.

### Where module code runs

A translated mission is the original `.scr` lifted to C, and it runs on the game's guest machine:
the same registers, x87 stack and 32-bit guest memory as the lifted game, bound by name when the
module is loaded. The scripting library and the C runtime that the `.scr` linked in are lifted
with it and stay inside the module; only the Windows imports cross into the client, slot by slot,
to the same rows the game's own imports use. So a mission behaves as the original's bytes do,
including its copy of the scripting library, and the game calls it as it called the `.scr`: at
guest addresses, through the exports and the virtual tables.

### Guest bases

Each module is lifted at a guest base of its own, so that its code and data addresses stay what the
lifted code expects and no two missions overlap: 1 MB slots from 0x60000000, the n-th `.scr` of
its folder (in byte order of the names) in slot n, 256 slots. `--base` places a module anywhere on a
64 KB boundary between 0x20000000 and 0x7F000000. Nothing else of the game's maps there
(`src/app/scripts/guest/GuestModuleAbi.h` says why).

### What the client does with a module

`GuestModules` answers LoadLibraryA, GetProcAddress and FreeLibrary for a `.scr` name. It loads the
library through the real loader, which makes every check of ModuleContract.md §5 on the file
before any of it runs; checks the guest image (version, range, dispatch table, import slots, and
the imports again against the allow-list); reserves the module's range of guest memory and copies
the image there; writes each import slot; binds the module to the client's runtime; answers the
module's guest addresses in the lifter's dispatch; and calls the DLL entry point with
DLL_PROCESS_ATTACH. GetProcAddress gives the guest addresses of `MissionSpecEntry` and
`ScriptEntry`. The last FreeLibrary detaches, releases the range and unloads the library. A module
that is refused is logged with its reason, LoadLibraryA returns 0 and the game goes on without it.

A mission's KERNEL32 imports that the game does not import itself (HeapReAlloc, HeapSize, TlsFree)
are answered by pcrecomp's win32hle rows, registered when the first module needs them.

## Limits

- **The lift runs in a container**: `docker run --rm --network none --cpus 1 --memory 4g`, under
  `nice -n 19`, with the patched copy of pcrecomp and the lifter's scripts mounted read-only. One lift runs at
  a time. A lift the container kills for memory (exit 137) ends that script with exit 3; Ski_TShoot
  was killed once while other jobs shared the machine and translated on the repeat.
- **`--jobs N`** (default 4) is how many of a module's C files are compiled at once, each under
  `nice -n 19`; it does not change the lift.
- **Time.** One measurement: X25_Ruse (a typical 377,000-byte script) took 67.7 s from the `.scr` to
  the installed module, lift and compile together, on the owner's laptop with other work running.
  At that rate 88 scripts take about an hour and three quarters when nothing is cached. Running
  `--all` again with every module current takes seconds (the record run below checked 87 modules and
  translated one in 73 s).
- **The kept lift is keyed on the lift's inputs, not on the compile.** `lift.key` hashes the
  rebased image, the pcrecomp revision `cmake/lifter/lift.sh` pins, the patch set (the series and
  each patch, `cmake/lifter/patches/`), the seeds, the file count and `run_lift.py`,
  `eh_frames.py` and the `Dockerfile`. Compile flags and `TRANSLATOR_VERSION` are not in it, which
  is right: they change only what is done with the lifted C, so a translator bump that changes the
  compile reuses every kept lift. The lift runs on a patched copy made from the pinned revision,
  so what the clone has checked out does not matter. One input of the lift is still missing from
  the key: the steps of `lift()` itself (the rtti and disassembly runs and the seed merge). After
  a change to those, delete the work folder's `lift.key` files, or a stale lift is reused.
- **No retries.** A script that fails stops with its exit status and its reason (below); nothing is
  run twice. `--all` records a refused script (exit 1), a bad input (2) or a mismatch (4) and goes on
  to the next; after a tool failure (3: Docker, the compiler, memory), which would repeat, it stops
  and records the remaining scripts as not run. Fix the cause and run the same command again: the
  modules already made are kept.
- **`--check` needs a reference trace** (`oracle/trace/FORMAT.md`) that this tool does not make. It
  is recorded on the original game: the original `.scr` run on the oracle input script by
  `oracle/run_oracle.sh` on a machine that can run the original (DESIGN §12.9, open). The module's
  own trace comes from the native client's oracle run (`--oracle`, `--input`). No reference trace has
  been recorded yet, so `--check` is tested on the trace fixtures only.
- **One host at a time.** A module is built for the host that runs the tool (its compiler, its
  `baseline-<host>.txt`); a library of another host is refused by the loader.

## How the client finds and loads a module

- **One root.** `$SFCOP_MISSION_MODULES` when it is set and not empty, else `mods/missions/` under
  the working directory (the per-user data folder). There is no search path and no fallback: the
  install and the current directory are never looked in.
- **The mapping.** The game's `LoadLibraryA("...\<Stem>.scr")` loads `<root>/<Stem>/<Stem>.dylib`
  (`.so`, `.dll`), where `<Stem>` is the file name of the argument without its directory and its
  `.scr`, in the case the game wrote. On a case-sensitive file system (Linux) the folder and the
  library must have exactly that case; macOS and Windows file systems usually do not care. A name
  that is not letters, digits, `_`, `-` or `.` (or starts with a dot) is refused.
- **A module that is not there** is the result Windows gives: `LoadLibraryA` returns 0 with
  `ERROR_MOD_NOT_FOUND`, and the client logs `[scripts] refused: <Stem>: not found` (any other
  refusal is logged with its own reason, `refused: <Stem>: ...`, and never with a path). The game
  goes on without that mission.
- **Reference count.** A module loaded again by the same name, in any case, is the same module with
  the same handle and one more reference. `FreeLibrary` takes one away; the last runs the entry point
  with `DLL_PROCESS_DETACH`, releases the module's guest memory and unloads the library.
- **The slot rule.** A module's guest base comes from its script's place in the folder it was
  translated from (above), not from its name. Two modules translated from different folders can
  therefore share a slot. The second is refused (`its guest image overlaps <first>'s`) for as long
  as the first is loaded, and loads once the first has been freed; give one of them a `--base` to
  have both at once. The shipped missions are one folder and never collide.

## Results for the shipped missions

All 88 scripts of the original game translate (macOS arm64, translator 1.0.0, pcrecomp at the
revision `cmake/lifter/lift.sh` pins). One command makes this table, writes it to
`<work>/results.md` and prints the totals:

```sh
python3 tools/scr2native/scr2native.py --all <the install folder> -o <modules root> [--work DIR]
```

It translates every `.scr` of the install's `Assets/Scripts`, one after another. A module that is
already installed is kept, without a lift, when its `module.json` names this script's hash and this
translator version and its library still passes the import and baseline checks (`kept`); anything
else is translated again. Bump `TRANSLATOR_VERSION` when a change to the tool changes what it
writes, so that every module is made again.

Imports counts the `.scr`'s Windows imports (all on the allow-list). Functions counts the lifted
functions in the module, the scripting library and the C runtime included. Seconds is the wall time of
the translation that made the module (lift and compile), where the tool recorded it: the record run
below translated X25_Ruse from scratch, and the other modules were made before the tool recorded
times (`-`). Whether each module behaves as its original is `--check`'s question.

| Script | Guest base | .scr bytes | Imports | Functions | Module bytes | Seconds | Outcome |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- |
| Bon_Heater | 0x60000000 | 389120 | 90 | 4169 | 8436256 | - | kept: module.json names this script and passes the checks |
| Bon_Hit | 0x60100000 | 389120 | 90 | 4157 | 8402848 | - | kept: module.json names this script and passes the checks |
| Bon_PieceAction | 0x60200000 | 389120 | 90 | 4135 | 8418664 | - | kept: module.json names this script and passes the checks |
| Gen_Playback | 0x60300000 | 360448 | 90 | 3800 | 7912416 | - | kept: module.json names this script and passes the checks |
| Met_10Patrol | 0x60400000 | 421888 | 90 | 4456 | 8973824 | - | kept: module.json names this script and passes the checks |
| Met_11ConvoyRaid | 0x60500000 | 397312 | 90 | 4117 | 8450792 | - | kept: module.json names this script and passes the checks |
| Met_12ConvoyEscort | 0x60600000 | 397312 | 90 | 4148 | 8468296 | - | kept: module.json names this script and passes the checks |
| Met_13Monster | 0x60700000 | 380928 | 90 | 4102 | 8351472 | - | kept: module.json names this script and passes the checks |
| Met_14Enigma | 0x60800000 | 385024 | 90 | 4100 | 8384192 | - | kept: module.json names this script and passes the checks |
| Met_15BaseDefense | 0x60900000 | 389120 | 90 | 4108 | 8417736 | - | kept: module.json names this script and passes the checks |
| Met_16ShipDefense | 0x60A00000 | 389120 | 90 | 4113 | 8434392 | - | kept: module.json names this script and passes the checks |
| Met_17Patrol | 0x60B00000 | 417792 | 90 | 4401 | 8972048 | - | kept: module.json names this script and passes the checks |
| Met_18HomeworldAssault | 0x60C00000 | 385024 | 90 | 4092 | 8383920 | - | kept: module.json names this script and passes the checks |
| Met_19Scan | 0x60D00000 | 397312 | 90 | 4284 | 8621312 | - | kept: module.json names this script and passes the checks |
| Met_1Scout | 0x60E00000 | 376832 | 90 | 3978 | 8215136 | - | kept: module.json names this script and passes the checks |
| Met_20SupriseR | 0x60F00000 | 380928 | 90 | 4061 | 8317112 | - | kept: module.json names this script and passes the checks |
| Met_21DistressCall | 0x61000000 | 385024 | 90 | 4098 | 8433640 | - | kept: module.json names this script and passes the checks |
| Met_22Diplomat | 0x61100000 | 397312 | 90 | 4280 | 8637672 | - | kept: module.json names this script and passes the checks |
| Met_23Quarters | 0x61200000 | 393216 | 90 | 4130 | 8467976 | - | kept: module.json names this script and passes the checks |
| Met_24Anomaly | 0x61300000 | 397312 | 90 | 4265 | 8637184 | - | kept: module.json names this script and passes the checks |
| Met_25SuperFleet | 0x61400000 | 385024 | 90 | 4058 | 8317032 | - | kept: module.json names this script and passes the checks |
| Met_26AsteroidAssualt | 0x61500000 | 401408 | 90 | 4288 | 8637928 | - | kept: module.json names this script and passes the checks |
| Met_27AsteroidDefense | 0x61600000 | 401408 | 90 | 4293 | 8654840 | - | kept: module.json names this script and passes the checks |
| Met_28Negotiation | 0x61700000 | 397312 | 90 | 4118 | 8549896 | - | kept: module.json names this script and passes the checks |
| Met_29Recharge | 0x61800000 | 389120 | 90 | 4127 | 8484168 | - | kept: module.json names this script and passes the checks |
| Met_2HoldingAction | 0x61900000 | 393216 | 90 | 4108 | 8384712 | - | kept: module.json names this script and passes the checks |
| Met_30Salvage | 0x61A00000 | 385024 | 90 | 4100 | 8367680 | - | kept: module.json names this script and passes the checks |
| Met_31Epicenter | 0x61B00000 | 393216 | 90 | 4235 | 8653064 | - | kept: module.json names this script and passes the checks |
| Met_3Ambushee | 0x61C00000 | 385024 | 90 | 4161 | 8369952 | - | kept: module.json names this script and passes the checks |
| Met_4Ambusher | 0x61D00000 | 376832 | 90 | 4031 | 8233344 | - | kept: module.json names this script and passes the checks |
| Met_5FleetAction | 0x61E00000 | 372736 | 90 | 4010 | 8298968 | - | kept: module.json names this script and passes the checks |
| Met_6Patrol | 0x61F00000 | 421888 | 90 | 4454 | 8973744 | - | kept: module.json names this script and passes the checks |
| Met_7BaseAssault | 0x62000000 | 389120 | 90 | 4098 | 8400616 | - | kept: module.json names this script and passes the checks |
| Met_8ShipAssault | 0x62100000 | 389120 | 90 | 4103 | 8417576 | - | kept: module.json names this script and passes the checks |
| Met_9PlanetaryAssault | 0x62200000 | 389120 | 90 | 4104 | 8400840 | - | kept: module.json names this script and passes the checks |
| Met_StarbaseConstruction | 0x62300000 | 389120 | 90 | 4169 | 8370224 | - | kept: module.json names this script and passes the checks |
| Mul_10Hockey | 0x62400000 | 397312 | 90 | 4300 | 8605280 | - | kept: module.json names this script and passes the checks |
| Mul_11Tinyfest | 0x62500000 | 380928 | 90 | 3981 | 8231752 | - | kept: module.json names this script and passes the checks |
| Mul_12Slugfest | 0x62600000 | 380928 | 90 | 3981 | 8231752 | - | kept: module.json names this script and passes the checks |
| Mul_13RandomBattle | 0x62700000 | 376832 | 90 | 3909 | 8147096 | - | kept: module.json names this script and passes the checks |
| Mul_14TimedBattle | 0x62800000 | 385024 | 90 | 3947 | 8230888 | - | kept: module.json names this script and passes the checks |
| Mul_16SuperFleet | 0x62900000 | 385024 | 90 | 4022 | 8249576 | - | kept: module.json names this script and passes the checks |
| Mul_17Monster | 0x62A00000 | 380928 | 90 | 4011 | 8232704 | - | kept: module.json names this script and passes the checks |
| Mul_18MonsterMash | 0x62B00000 | 380928 | 90 | 3889 | 8113432 | - | kept: module.json names this script and passes the checks |
| Mul_19ScanHappy | 0x62C00000 | 385024 | 90 | 4005 | 8331592 | - | kept: module.json names this script and passes the checks |
| Mul_1Free4All | 0x62D00000 | 376832 | 90 | 3886 | 8113088 | - | kept: module.json names this script and passes the checks |
| Mul_20Starbase | 0x62E00000 | 368640 | 90 | 3945 | 8214376 | - | kept: module.json names this script and passes the checks |
| Mul_2BaseAssault | 0x62F00000 | 385024 | 90 | 4014 | 8282344 | - | kept: module.json names this script and passes the checks |
| Mul_3Battlefests | 0x63000000 | 389120 | 90 | 4103 | 8367784 | - | kept: module.json names this script and passes the checks |
| Mul_4Tourneyfest | 0x63100000 | 393216 | 90 | 4098 | 8367864 | - | kept: module.json names this script and passes the checks |
| Mul_5BattlefestLite | 0x63200000 | 380928 | 90 | 3981 | 8231752 | - | kept: module.json names this script and passes the checks |
| Mul_6TourneyfestLite | 0x63300000 | 380928 | 90 | 3977 | 8215352 | - | kept: module.json names this script and passes the checks |
| Mul_7Tourney | 0x63400000 | 376832 | 90 | 3888 | 8113408 | - | kept: module.json names this script and passes the checks |
| Mul_9TeamAssault | 0x63500000 | 380928 | 90 | 3998 | 8248808 | - | kept: module.json names this script and passes the checks |
| Mul_Intruder | 0x63600000 | 393216 | 90 | 4228 | 8569952 | - | kept: module.json names this script and passes the checks |
| Mul_TWoK | 0x63700000 | 372736 | 90 | 4021 | 8249856 | - | kept: module.json names this script and passes the checks |
| Ski_1Free4All | 0x63800000 | 364544 | 89 | 3843 | 8045520 | - | kept: module.json names this script and passes the checks |
| Ski_GreenNGold | 0x63900000 | 372736 | 90 | 3980 | 8182504 | - | kept: module.json names this script and passes the checks |
| Ski_Hood | 0x63A00000 | 380928 | 90 | 3973 | 8248320 | - | kept: module.json names this script and passes the checks |
| Ski_SuperFleet | 0x63B00000 | 380928 | 90 | 4055 | 8333448 | - | kept: module.json names this script and passes the checks |
| Ski_SupriseReversed | 0x63C00000 | 372736 | 90 | 3946 | 8181368 | - | kept: module.json names this script and passes the checks |
| Ski_TShoot | 0x63D00000 | 372736 | 90 | 3959 | 8165264 | - | kept: module.json names this script and passes the checks |
| Ski_TWoK | 0x63E00000 | 368640 | 90 | 3964 | 8198528 | - | kept: module.json names this script and passes the checks |
| Ski_WarOfRoses | 0x63F00000 | 364544 | 90 | 3923 | 8114600 | - | kept: module.json names this script and passes the checks |
| Ski_Wreck | 0x64000000 | 372736 | 90 | 3932 | 8131392 | - | kept: module.json names this script and passes the checks |
| Tut_X25_1 | 0x64100000 | 368640 | 90 | 3916 | 8163840 | - | kept: module.json names this script and passes the checks |
| Tut_X25_2 | 0x64200000 | 368640 | 90 | 3919 | 8114384 | - | kept: module.json names this script and passes the checks |
| Tut_X25_3 | 0x64300000 | 368640 | 90 | 3929 | 8180752 | - | kept: module.json names this script and passes the checks |
| Tut_X25_4 | 0x64400000 | 368640 | 90 | 3914 | 8097712 | - | kept: module.json names this script and passes the checks |
| Tut_X25_Command190 | 0x64500000 | 368640 | 90 | 3918 | 8213432 | - | kept: module.json names this script and passes the checks |
| Tut_X25_Command290 | 0x64600000 | 368640 | 90 | 3954 | 8181560 | - | kept: module.json names this script and passes the checks |
| Tut_X25_Command590 | 0x64700000 | 389120 | 90 | 4088 | 8813496 | - | kept: module.json names this script and passes the checks |
| Tut_X25_Science310 | 0x64800000 | 385024 | 90 | 4094 | 8433784 | - | kept: module.json names this script and passes the checks |
| Tut_X25_Weapons380 | 0x64900000 | 385024 | 90 | 4068 | 8350376 | - | kept: module.json names this script and passes the checks |
| Tut_X25_Weapons480 | 0x64A00000 | 376832 | 90 | 4036 | 8299816 | - | kept: module.json names this script and passes the checks |
| X25_BugHunt | 0x64B00000 | 389120 | 90 | 4179 | 8436576 | - | kept: module.json names this script and passes the checks |
| X25_CatchThief | 0x64C00000 | 385024 | 90 | 4120 | 8368648 | - | kept: module.json names this script and passes the checks |
| X25_DeadEnd | 0x64D00000 | 393216 | 90 | 4301 | 8589120 | - | kept: module.json names this script and passes the checks |
| X25_Face | 0x64E00000 | 389120 | 90 | 4145 | 8848400 | - | kept: module.json names this script and passes the checks |
| X25_HellBack | 0x64F00000 | 389120 | 90 | 4173 | 8419872 | - | kept: module.json names this script and passes the checks |
| X25_Hire | 0x65000000 | 393216 | 90 | 4229 | 8487744 | - | kept: module.json names this script and passes the checks |
| X25_Honor | 0x65100000 | 385024 | 90 | 4136 | 8369152 | - | kept: module.json names this script and passes the checks |
| X25_LawNOrder | 0x65200000 | 385024 | 90 | 4091 | 8334672 | - | kept: module.json names this script and passes the checks |
| X25_Legend | 0x65300000 | 393216 | 90 | 4263 | 8538368 | - | kept: module.json names this script and passes the checks |
| X25_Legendary | 0x65400000 | 385024 | 90 | 4054 | 8300432 | - | kept: module.json names this script and passes the checks |
| X25_Ruse | 0x65500000 | 376832 | 90 | 4063 | 8284208 | 67.7 | translated |
| X25_TheTest | 0x65600000 | 372736 | 90 | 4001 | 8199680 | - | kept: module.json names this script and passes the checks |
| X25_Travel | 0x65700000 | 389120 | 90 | 4161 | 8815888 | - | kept: module.json names this script and passes the checks |

88 scripts, 88 modules (87 kept, 1 translated), 0 not translated; modules 736.6 MB, work files 1.5 MB, this run 73 s.

The record run translated one script and kept 87, and left 1.5 MB of work files (the lift and the
objects are removed once a module is installed).

## Compile flags: -O1, -O2 and the direct runtime view

Translator 1.0.0 compiled at `-O1`; 1.1.0 at `-O2`; 1.2.0 adds `runtime/module_rt_direct.h` to the
lifted files; 1.2.1 makes the glue's `sfcop_bind` refuse a later bind to other addresses, which the direct
view relies on, and passes `-fno-lto` (the glue only changes, so the 1.2.0 measurements below hold
for it). Measured on a sample of 10 modules (the smallest and the largest script, campaign,
bonus and skirmish missions, and the oracle's skirmish), each lifted once and compiled three
times from the same lift, macOS arm64, `--jobs 4`, with other work on the machine (load averages
18 to 36, so the compile times are within noise of each other):

| Module | Functions | Compile s: -O1 | -O2 | 1.2.0 | Bytes: -O1 | -O2 | 1.2.0 | 1.2.0 vs -O1 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Gen_Playback | 3800 | 14.6 | 15.0 | 12.5 | 7912416 | 7928912 | 6343760 | -19.8 % |
| Met_10Patrol | 4456 | 23.0 | 20.2 | 18.2 | 8973824 | 8990352 | 7256592 | -19.1 % |
| Met_11ConvoyRaid | 4117 | 18.4 | 17.8 | 15.8 | 8451032 | 8451064 | 6783352 | -19.7 % |
| Met_12ConvoyEscort | 4148 | 15.0 | 17.3 | 17.3 | 8468552 | 8468568 | 6800856 | -19.7 % |
| Bon_Heater | 4169 | 15.4 | 17.4 | 18.9 | 8436256 | 8452768 | 6801552 | -19.4 % |
| Bon_Hit | 4157 | 18.9 | 17.9 | 19.3 | 8402848 | 8419360 | 6768144 | -19.5 % |
| Bon_PieceAction | 4135 | 18.7 | 17.3 | 15.9 | 8418664 | 8435176 | 6783960 | -19.4 % |
| Met_1Scout | 3978 | 14.5 | 17.5 | 16.0 | 8215376 | 8231888 | 6613712 | -19.5 % |
| Ski_1Free4All | 3843 | 14.0 | 16.6 | 15.0 | 8045792 | 8045792 | 6444112 | -19.9 % |
| OracleSkirmish | 3607 | 17.2 | 24.2 | 16.4 | 7724376 | 7724376 | 6188744 | -19.9 % |
| mean | | 17.0 | 18.1 | 16.5 | 8304913 | 8314825 | 6678478 | -19.6 % |

`-O2` alone leaves the size where it was; the direct view takes a fifth off every module. The lift
(22 to 31 s a module on the same machine) is the same for all three. Two 1.2.0 builds of the same
lift are byte-identical, and a 1.2.0 module has the same undefined and exported symbols as its
`-O1` build. The table above for all 88 scripts was made at 1.0.0; for the whole set at 1.2.0 the
sizes are extrapolated from this sample (about 590 MB against 737 MB), not measured.

## What C8 fixed, and why

- **Names are the original's.** The allow-list holds decorated VC6 names and `DLL!Name`, the only
  names the SDK and a `.scr` carry. Host modules reach them through the `sfcop_import:` prefix, so
  the original's `_free` never meets the host's `_free`.
- **The list is generated from the install.** `gen_allowlist.py` reads `ScriptInterfaceR.lib`,
  builds the SDK's Single template with the VC6 container and reads its link map, objects and
  import table. Only the list is committed (DESIGN §12.5). The header names the SDK version, the
  scripting library version and the library's hash, so a list made from another SDK shows.
- **Checks read the file, not the running module.** The version tag is plain data and the imports
  are the file's undefined symbols, so a refused module never runs a static initializer.
- **The loader refuses at the first failing check**, in a fixed order, with one message format
  shared by the C++ loader and the Python checker.

## Regenerating the host baseline list

```sh
python3 tools/scr2native/gen_baseline.py          # write src/app/scripts/baseline-<host>.txt
python3 tools/scr2native/gen_baseline.py --check  # exit 1 if the committed list differs
```

It compiles probes with the module flags (the lifter's runtime header and macros, the glue, the
math and string functions the lifted code calls, a stack frame and a pattern fill the compiler
turns into library calls) and lists their undefined symbols, less the runtime the module binds by
name. It needs the host compiler and the pcrecomp clone; it compiles against the same patched copy
as scr2native (`--work`, by default scr2native's).

## Regenerating the allow-list

```sh
python3 tools/scr2native/gen_allowlist.py                 # build the template, then write the list
python3 tools/scr2native/gen_allowlist.py --build-dir DIR # reuse a template build
```

It needs the install, the `sfcop-msvc6` image and the VC6 Lib set under `tools/vc6-lib/`. The
template build goes to `$SFCOP_WORK/scr2native/allowlist/`. The same build and install give the
same file byte for byte; a change to the list in a pull request is a change of SDK, toolchain or
rule, and says which.

`toolchain/scr/` finds the install relative to its own checkout, so in a git worktree (where
the link to the install is absent) the template build has to be made from the main checkout
(`toolchain/scr/test_template.sh` or `toolchain/build_scr.sh`) and passed with `--build-dir`.

Three names in the committed list are not the generator's, and the generator has no input for extra
names:

- `KERNEL32.DLL!GetEnvironmentVariableA`, which 87 of the 88 shipped scripts import, was added by
  hand with a note above it (the Lib set the template links lacks the function).
- `OSSPECIFIC95.DLL!OS_InterlockedCompareExchange` and `OSSPECIFIC98.DLL!OS_InterlockedCompareExchange`,
  which every shipped script looks up at start-up for its lock, were added by hand with a note in
  the header.

A regeneration drops them, and `tests/test_module_imports.py::test_allowlist_keeps_the_hand_added_name`
or `::test_allowlist_keeps_the_hand_added_osspecific_names` fails until they are put back.
