# src/app: the client's front door

What runs before the game: the command line, the per-user data folder, finding the install and
verifying it. DESIGN §5.1, §5.3, §6.6, §6.8 and §8 define the behavior.

| File | What it does |
| --- | --- |
| `CommandLine.h` | Parses the flags of DESIGN §6.8 from one table, which also generates `--help`. A caller can add its own flags (`ExtraFlag`), which are listed and passed through unchecked. An unknown flag, a missing or malformed value, a flag given twice, `--windowed` with `--fullscreen`, or `--oracle` without `--input` is bad input, with the flag named. `--input` alone is allowed: a script with no trace is a menu run. `--logs DIR` places the files the game writes beside its exe (its `.txt` and `.log` files) in DIR instead of `logs/` under the data folder |
| `oracle/` | `--oracle TRACE`, `--record-trace OUT` and `--logs DIR`: the replay oracle around the game (`oracle/README.md`). The mission's log leaves the data folder through `--logs`; the run waits for its `END`, makes the trace with `oracle/trace/mklog.py`, compares with `compare.py` and exits 0 or 4 |
| `input/` | `--input SCRIPT`: the steps file read, played frame by frame and injected through a `Window` wrapper, so the game takes it as real input (`input/README.md`). A bad file, or a wait in it that runs out, ends the run with exit 2 |
| `ExitCodes.h` | 0 success, 1 the game started and then failed, 2 bad input, 3 install not found or refused, 4 oracle mismatch |
| `Install.h` | Reads the hash list (sha256sum's format), picks out the install's entries, finds the install (`--install`, else the remembered path, else the caller's and the platform's usual folders, else asks once) and verifies every listed file by SHA-256, read in 1 MiB pieces with a progress callback. The first missing, unreadable or different file, in list order, is refused by name with exit 3, and the message says to reinstall and re-patch to 2.5.5.2 |
| `DataDir.h` | The per-user data folder on each platform (`--data` overrides it), its subfolders `settings/`, `saves/`, `replays/`, `logs/` and `mods/`, created on first use, and `install.txt`, the install path remembered after the first verified run |
| `Startup.h` | All of the above in order, for `main()` |
| `Sha256.h` | SHA-256, fed in pieces |
| `HostPlatform.h` | The platform and its environment, one file per platform, passed in so every platform's rules are tested everywhere |
| `Files.h` | **Temporary.** The few file operations these need, until the Store component has a real backend (B5); the integrator then replaces it with Store's in its own pull request. `FilesMock.h` holds the in-memory test double |

The install is only ever read. Nothing here writes outside the data folder.

## Wiring into `main.cpp`

Done at gate G0 (2026-11-04). `main.cpp` calls `startClient` with `consoleStartup(SFCOP_HASH_LIST,
kBootFlags, {SFCOP_INSTALL_DIR})`: the build's install is tried after the remembered one and
before the platform's usual folders, and when none is found the player is asked on the terminal,
except with `--headless`. The temporary boot platform's own flags are the `kBootFlags` extras, so
`--help` lists them and they come back in `start.options.passThrough`; `main.cpp` turns `start`
into the boot platform's arguments until Wave 1 replaces that platform. `SFCOP_HASH_LIST` is
defined by `cmake/sfcop_build_config.h.in`.

`kDataSubdirs` is built from `Store`'s `userFolderName` (`src/platform/Store/Store.h`), so the
per-user subfolders have one list. `Files.h` stays until a real `Store` backend lands (B5); the
swap to `Store` is the integrator's, in its own pull request, once that backend exists.

## Hash list

The repository's list has its root above the install: it also names the server kit. The install's
entries are those under the folder of the `StarFleetOP.exe` entry with the most entries beneath
it, taken relative to that folder.

## Tests

`*Test.cpp` beside each unit, run by `ctest --preset macos-arm64`. The invented fixture install
is `tests/fixtures/install/` with its list `tests/fixtures/install.sha256`; none of it is a
retail file. One test verifies the real install against the repository's list and is skipped
unless `SFCOP_INSTALL_DIR` names an install.
