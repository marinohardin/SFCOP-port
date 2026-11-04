# scr2native

The mission script translator (DESIGN §5.9 path (b)): it takes a compiled `.scr` and produces a
native mission module the native build loads. This file is the design. The translator itself is
work package A3; what exists now is the contract it builds to and the import allow-list.

| What | Where | State |
| --- | --- | --- |
| The module contract: entry points, version tag, import rule, files, refusals | `src/app/scripts/ModuleContract.md` | Fixed (C8) |
| The loader interface and its mock | `src/app/scripts/ModuleLoader.h`, `ModuleLoaderMock.h` | Fixed (C8); the real loader is A3 |
| The import allow-list and its generator | `src/app/scripts/allowlist.txt`, `gen_allowlist.py` here | Fixed (C8) |
| The translator | `scr2native` | A3 |

## What a `.scr` is

A `.scr` is a 32-bit Windows DLL built with VC6 from the SDK's templates. It links the scripting
library (`ScriptInterfaceR.lib`), the SDK libraries it depends on and the static C runtime
(`/MT`) into itself, exports `MissionSpecEntry` and `ScriptEntry`, and imports only Windows system
DLLs (KERNEL32 and a few functions of USER32 and ADVAPI32). The game reaches the mission only
through those two exports, then through the `tScriptEntry` virtual table and the callbacks the
game installs through it. `toolchain/build_scr.sh` builds one from source and
`toolchain/scr/check_scr.py` compares it with the shipped scripts.

## The pipeline

```
scr2native <Name>.scr --out <modules root> [--check]

1. read      verify the .scr (PE32 DLL, exports MissionSpecEntry and ScriptEntry), hash it
2. lift      the same lifter as the game, with the .scr as input: every function to C, its
             import table kept as named imports
3. bind      functions that match the scripting library, the SDK libraries or the C runtime
             (by the same fuzzy SDK matching the game's analysis uses) are dropped and become
             imports of the original's name; the Windows imports keep their DLL!Name;
             every import is emitted as the undefined symbol sfcop_import:<name>
4. check     every import name against src/app/scripts/allowlist.txt; a foreign one stops the
             translation with the loader's refusal message (gen_allowlist.py --check is this step)
5. compile   the lifted C with the host compiler, in the guest-mode of the game's lift, plus the
             sfcop_module_abi tag (ModuleLoader.h hostAbiTag())
6. package   <modules root>/<Name>/<Name>.<dylib|so|dll> and module.json
             (name, source_sha256, translator_version)
```

`--check` holds the module to the original: the original `.scr` runs in the 32-bit Windows build
(which loads `.scr` files unchanged), the module runs in the native build, both on the same oracle
input script, and `oracle/trace/` compares the two traces. A module whose trace differs is not
written.

## What A3 implements

- The translator above, as a command under `tools/scr2native/`, with the lifter driver shared
  with the game's lift.
- The real `ModuleLoader` for each host: reading a module directory into a `ModuleImage` from the
  file alone (Mach-O, ELF, PE), `checkImage()`, then `dlopen`/`LoadLibrary` and the two entry
  points.
- The host side of the binding: the native build exports each allow-list symbol as
  `sfcop_import:<name>`, implemented by the decompiled or lifted scripting library and by the
  HLE surface for the `DLL!Name` imports.
- The host baseline list (ModuleContract.md §3 rule 2) for each platform, generated from the host
  toolchain the way `allowlist.txt` is generated from the SDK.
- A test per shipped mission: through the translator, it drives the oracle to the original's trace
  (DESIGN §5.9).

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
`<private>/` is absent) the template build has to be made from the main checkout
(`toolchain/scr/test_template.sh` or `toolchain/build_scr.sh`) and passed with `--build-dir`.
