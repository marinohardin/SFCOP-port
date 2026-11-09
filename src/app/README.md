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
| `DataDir.h` | The per-user data folder on each platform (`--data` overrides it), its subfolders `settings/`, `saves/`, `replays/`, `logs/` and `mods/`, created on first use, `install.txt`, the install path remembered after the first verified run, and `mission-modules.txt`, the folder of translated mission modules, which the player writes once (below) |
| `Startup.h` | All of the above in order, for `main()` |
| `Sha256.h` | SHA-256, fed in pieces |
| `HostPlatform.h` | The platform, its environment and the running program's own path, one file per platform, passed in so every platform's rules are tested everywhere |
| `Bundle.h` | Run from `SFCOP.app`, the hash list is the bundle's copy in `Contents/Resources`; a build run in place reads the source tree's list it was compiled with (`SFCOP_HASH_LIST`) |
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

## Building the app

On macOS, one command builds the client with the lift and writes `dist/SFCOP.app` at the
repository root, which git ignores:

```
sh scripts/dist.sh [BUILD_DIR]
```

`BUILD_DIR` defaults to `build/macos-arm64`, the preset's directory; pass an existing lift build
directory to reuse it. A second run with nothing changed rebuilds nothing and rewrites the bundle.
The build itself makes the bundle in `<build>/dist/SFCOP.app` (target `sfcop_app`,
`cmake/SfcopBundle.cmake`, `packaging/macos/`); the script copies it out.

The bundle holds the client, `libSDL2-2.0.0.dylib` and `libSDL3.dylib` in `Contents/Frameworks`
(loaded through `@rpath`, nothing points at the build directory), the hash list and the icon in
`Contents/Resources`, and nothing from the original game: `packaging/macos/check_bundle.py`
refuses a bundle with a file whose hash is in the hash list, a mission module, or any file that
is not the client's own. It is signed ad hoc, for its owner's Mac; it is not a release (DESIGN
§12.2). `packaging/macos/SFCOP.icns` is a placeholder icon (`make_icon.py` draws it); replace the
file to change it.

Started from Finder, the app finds the install as a terminal run does (remembered in
`install.txt`, else the install the build was made from, else the usual places) but cannot ask
for one, and it writes what the client prints to `logs/launch.log` in the data folder. The mission
list needs the translated modules: a double click has no environment, so name their folder once
in the data folder,

```
echo /path/to/missions/modules > ~/Library/Application\ Support/SFCOP/mission-modules.txt
```

and every later run finds them there. `SFCOP_MISSION_MODULES` still wins when it is set, and
without either the client looks in `mods/missions` in the data folder.
