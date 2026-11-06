# src/hle/kernel: KERNEL32's files, directories, module path and .ini profiles

The game's 49 KERNEL32 imports that touch files (`surface.tsv`: the file, directory, search,
module-path and `.ini` rows), on Store. Threads, synchronisation, memory, time and the file
mappings are B6's rows and are not here.

| File | What |
|---|---|
| `Kernel.h` | The module's interface and the game's path model (top of the file) |
| `Paths.cpp` | Paths, the current directory, the module path, drives and volumes |
| `Files.cpp` | Opening, reading, writing, deleting, moving and copying files; directories; attributes |
| `Find.cpp` | `FindFirstFile`/`FindNextFile` over the install and the per-user copies together |
| `Ini.cpp` | The `.ini` profile calls |
| `Text.h`, `Text.cpp` | The game's ANSI (Latin-1) and UTF-16 strings, and the Win32 buffer answers |
| `KernelBridge.h`, `KernelBridge.cpp` | The module as the C shim calls it, one per process, behind a lock |
| `LogsStore.h`, `LogsStore.cpp` | A Store whose per-user `logs/` lives in a folder of its own (the client's `--logs DIR`); `sfcop_hle_kernel_attach_logs` attaches the module on it |
| `KernelGuest.c` | The guest shims, `sfcop_hle_kernel[]` (built only with the lift) |
| `*Test.cpp`, `KernelTestFolders.h`, `KernelTestStores.h` | Doctests; `KernelTestFolders.h` makes an install and a per-user folder for a test, `KernelTestStores.h` a Store that records or throws |
| `KernelBridgeTesting.h` | Test-only: attaches the bridge to a Store the test built |
| `boot_proof.sh` | The boot to the CD-key prompt on these shims (below) |

## Where the game's files are

The game sees one folder, `C:\` with `StarFleetOP.exe` in it. Reads find the per-user copy of a
file first, then the install's; every write goes to the per-user copy, so the install is only read.
The per-user copy of a path under the game folder is:

| Game path | Per-user path |
|---|---|
| `saves\...`, `replays\...`, `mods\...`, `settings\...`, `logs\...` | the same path (`saves/...`) |
| any other `.txt` or `.log` file | `logs/` and the same path (`QAReport.txt` is `logs/QAReport.txt`) |
| anything else | `settings/` and the same path (`ClientSettings.gf` is `settings/ClientSettings.gf`) |
| a bare `.ini` name (the Windows directory on Windows) | `settings/windows/` and the name; a read that finds nothing there falls back to the install's game folder |

The five folders `saves`, `replays`, `mods`, `settings` and `logs` are per-user only: the install
ships none of them, so a lookup under one never asks the install (a test records the Store calls
to show it). Nor does any write row reach the install (`NoInstallWriteTest.cpp`).

A file only in the install cannot be deleted or renamed (`ERROR_ACCESS_DENIED`); a file opened to
change in place is copied to its per-user path first. The game is told the install's files are
writable (`FILE_ATTRIBUTE_ARCHIVE`), since a write to one succeeds.

## Handles

The file and search handles the game holds are entries of win32hle's handle table, of two types
of `KernelGuest.c`'s own (24 and 25), holding the module's id. A handle row given a handle the
module did not make (a standard handle, a file another module opened) goes to win32hle's own row.

Until the guest runtime attaches the module to the client's folders
(`sfcop_hle_kernel_attach`), the shim attaches it on its first call to the game folder (drive
`C:` as the host maps it) and the per-user folder the client uses without `--data`
(`src/app/DataDir.h`).

## The boot proof

`boot_proof.sh BUILD OUT` runs the lift build's `sfcop --headless --watchdog 60` twice on a clone
of `BUILD/gameroot` (once plain, once with `--trace`), with the wall clock pinned to 15:38 (the time
of day `src/app/boot/results/` shows reaching the prompt) and `HOME` in `OUT`, so the per-user folder
is `OUT/home/Library/Application Support/SFCOP`. It records every game file's modification time
before and after. From the repository root, on macOS:

```sh
src/hle/kernel/boot_proof.sh "$SFCOP_WORK/B5/build-lift" "$SFCOP_WORK/B5/proof2"
```

Re-run on 2026-11-04 on the branch merged with the contracts (the build generates the registration
of every shim table, so no linker flag is needed), and by `tools/boot_probe.sh` with the clock at
15:38:00 (background `space21`): 3 of 3 boots reach the CD-key prompt (5.55 s, 5.52 s, 5.53 s,
exit 0):

```
tools/boot_probe.sh -s BUILD/gameroot -w 15 -t 1793749080 3 -- BUILD/sfcop --headless --watchdog @WATCHDOG@ --game @GAME@
run 1 at 15:38:00: prompt 5.546 (exit 0) space21.mod
run 2 at 15:38:00: prompt 5.522 (exit 0) space21.mod
run 3 at 15:38:00: prompt 5.527 (exit 0) space21.mod
```

and `boot_proof.sh`'s summary:

```
plain run exit: 0 (0: the CD-key prompt)
plain run: [mark] reached 0x00484102 (CD-key prompt) at 5.546 s (last import RegOpenKeyExA)
traced run exit: 0
trace lines from the kernel and registry modules: 97
game files modified during the runs: 1
game folder modification times: CHANGED
files in the per-user folder: 0
```

In the temporary boot host the spike's tables are registered first and win where both answer a
name, so this module answers only what the spike does not. Of the rows the boot calls, the module
answers `RegOpenKeyExA` (57 calls), `FindFirstFileA` (32), `GetPrivateProfileIntA` (6),
`GetPrivateProfileStringA` and `SetCurrentDirectoryA` (1 each). The spike answers `CreateFileA`,
`CreateFileW` and `FindFirstFileW`, so the file the game writes, `Above8BitTextures.txt` (opened
with `OPEN_ALWAYS`), is written by the spike into the game folder (the one changed file), not by
this module into `logs/`. The first proof (before the spike's tables won), with all of these rows
answered here, had 368 trace lines, no game file changed and `Above8BitTextures.txt` in the
per-user `logs/`. The failed opens are the game's own probes: loose fonts (they are in
`Assets/fonts.zip`), `uq3.txt`, optional driver DLLs and folders the install does not have. B6's
host registers the modules first, which restores the first result.
