# src/app: the client's front door

What runs before the game: the command line, the per-user data folder, finding the install and
verifying it. DESIGN §5.1, §5.3, §6.6, §6.8 and §8 define the behavior.

| File | What it does |
| --- | --- |
| `CommandLine.h` | Parses the flags of DESIGN §6.8 from one table, which also generates `--help`. A caller can add its own flags (`ExtraFlag`), which are listed and passed through unchecked. An unknown flag, a missing or malformed value, a flag given twice, `--windowed` with `--fullscreen`, or `--oracle` without `--input` is bad input, with the flag named. `--input` alone is allowed: a script with no trace is a menu run. `--logs DIR` places the files the game writes beside its exe (its `.txt` and `.log` files) in DIR instead of `logs/` under the data folder. `--render-scale N` sets the Direct3D targets' render scale (`hle/ddraw4`, `fix_render_scale`): 0, the default, follows the window's pixel density, 1 keeps the game's own size, N > 1 is the most it may be; `--oracle` and `--record-trace` runs always use 1 |
| `oracle/` | `--oracle TRACE`, `--record-trace OUT` and `--logs DIR`: the replay oracle around the game (`oracle/README.md`). The mission's log leaves the data folder through `--logs`; the run waits for its `END`, makes the trace with `oracle/trace/mklog.py`, compares with `compare.py` and exits 0 or 4 |
| `input/` | `--input SCRIPT`: the steps file read, played frame by frame and injected through a `Window` wrapper, so the game takes it as real input (`input/README.md`). A bad file, or a wait in it that runs out, ends the run with exit 2 |
| `ExitCodes.h` | 0 success, 1 the game started and then failed, 2 bad input, 3 install not found or refused, 4 oracle mismatch |
| `Install.h` | Reads the hash list (sha256sum's format), picks out the install's entries, finds the install (`--install`, else the remembered path, else the caller's and the platform's usual folders, else asks once) and verifies every listed file by SHA-256 except the boot stub `SFCOP.exe` and the SDK's `.lib` files (`runtimeEntries`), read in 1 MiB pieces with a progress callback. The first missing, unreadable or different file, in list order, is refused by name with exit 3, and the message says to reinstall and re-patch to 2.5.5.2 |
| `Import.h` | `--import DIR`: copies the installed game into the client's own folder (below, "Importing the game"), and `--verify-import` checks that copy again. A library with progress and cancel callbacks for the launcher; `ImportCommand.h` is the command line's thin caller, `ImportFiles.h` the file operations it needs (`ImportClone_<platform>.cpp` the copy-on-write clone) |
| `DataDir.h` | The per-user data folder on each platform (`--data` overrides it), its subfolders `settings/`, `saves/`, `replays/`, `logs/` and `mods/`, created on first use, `game/`, the imported game (`gameDirFor`), `install.txt`, the install path remembered after the first verified run (`game` for the imported copy), and `mission-modules.txt`, the folder of translated mission modules, which the player writes once (below) |
| `Startup.h` | All of the above in order, for `main()`, then the fix switches: their defaults, `settings/fixes.gf` in the data folder when it exists, then each `--fix`, installed with `fixes::install` before the game starts; each problem with a switch (an unknown name, a bad value, a build switch asked for in its other position) is printed once and the run goes on (`src/fixes/README.md`) |
| `launcher/` | The launcher (below, "The launcher"): the first-run screens that import the game, and `--launcher` on demand. A state machine tested without a window (`Launcher.h`), the import library on a worker thread (`LauncherJobs.h`), its setup (`LauncherSetup.h`), every string (`LauncherText.h`), and the SDL3 window with Dear ImGui that draws it (`LauncherWindow.h`), driven by a steps file for captures (`LauncherSteps.h`) |
| `Sha256.h` | SHA-256, fed in pieces |
| `ServiceHosts.h` | The hosts of the Dynaverse stand-ins, from the `services` object of `servers.json` in the data folder, as the alias map the winsock module resolves the original's service host names by (`src/net/DIRECTORY_API.md`, "Pointing the client at the stand-ins"). With no `servers.json`, or a service it does not name, the host is a reserved name that resolves nowhere: the client fails closed and reaches no one's server until the file names the services (the integrator's decision; the original hosts are gone). A malformed file is reported on stderr and has the same effect |
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

## The launcher

When it opens depends on the kind of build:

- **A package build** (`SFCOP_PACKAGE_BUILD`, the release packages): a run with a window and no
  game set up (no `--install`, and no `install.txt` naming a folder that holds the game) opens
  the launcher before any other place is tried. The player is prompted and the game imported; a
  usual place that holds the game is a suggestion on Welcome, never used without asking.
- **A developer build** (any other): the order stays `--install`, `install.txt`, the build's own
  install, the platform's usual places. The launcher opens only where the terminal question
  would be asked on a windowed run, so every developer and oracle run is as before.

In both, `--launcher` opens it first, on Ready when a game is set up. `--headless`, `--input`,
`--oracle` and `--install` runs never open it, and `--launcher` with any of them, or with
`--import` or `--verify-import`, is bad input. `startClient` calls `StartupConfig::launcher`
after the data folder is made (`StartupConfig::packageBuild` says which build it is); `main.cpp`
passes the window, and a build with no compiled-in hash list is a package build.

- **Welcome.** The installed game's folder, chosen in the system's folder picker, dropped on the
  window or typed. The platform's usual folders that exist are suggested, each marked when the
  game is in it. On Linux with no desktop portal (no session bus) and no `zenity`, there is no
  folder picker; Welcome says so and the path field and dropping are the ways in.
- **Working.** The phase in words, the file, a bar of bytes while copying and checking and an
  indeterminate one otherwise, and Cancel. The import library runs on a worker thread; cancel
  is an atomic flag it polls, and the copy it was making is removed as with Ctrl-C.
- **Result.** Every status of `importGame` and `verifyImportedGame` has its screen: the
  report's message as written, its lists scrollable, and the actions that fit (Try again,
  Choose another folder, Import the game again, Back, Quit).
- **Ready.** After an import, `install.txt` names the copy (`game`). Play starts the game in the
  same process; Check game files runs `verifyImportedGame` and lists any file that differs;
  Change game files goes back to Welcome, and the game in use stays until a new import succeeds.

One job runs at a time, and closing the window during one cancels it and waits for it before the
window goes. An import that completes anyway in that time is not remembered (`install.txt` is
left as it was): the player closed the window before seeing it succeed, so the next start asks
again rather than starting a game they did not see imported. The window (SDL3, Dear ImGui on an SDL renderer, no shaders) is destroyed and the
video subsystem released before the game opens its own. With no display it cannot open: the
terminal question is asked instead, and `--launcher` exits 1.

`--launcher-steps FILE` drives the window from a steps file instead of a player, for captures
(`launcher/LauncherSteps.h`): `wait welcome|working|result|ready`, `frames N`, `choose PATH`,
`type TEXT`, `press NAME`, `close` and `shot FILE`. The file is read when the launcher opens, so
a run that never opens it never reads it; a file that cannot be read or holds a wrong step, or a
step the screen cannot do, ends the launcher with exit 2 and the reason named.

## Importing the game

`sfcop --import DIR [--data DIR]` copies a player's installed game into the client's own folder
and exits: 0 imported, 3 the folder refused, 1 the import could not finish. Later runs start from
the copy with no `--install`. A launcher calls the same `importGame` (`Import.h`).

1. **Find.** DIR itself, or the first folder below it, breadth first and in name order, that holds
   `StarFleetOP.exe` (any case), at most 8 levels down and 20,000 folders. The search never follows
   a link to a folder (a Wine prefix links the whole disk); DIR itself may be a link, and inside
   the install found every link is followed. Among up to 8 installs found, the first whose
   executable is 2.5.5.2 wins. A disc image, installer or archive chosen as a file, or a folder of
   InstallShield cabinets, is refused with what to do instead. So is the client's own copy: `game`,
   `game.partial` or `game.old`, or a folder in one, reached as it is or through a link, `..` or
   another case of ASCII letters (a Windows short 8.3 name is not recognised), since the import
   removes the last two before it copies; found below DIR, the copy is passed over. Inside the
   install found, a folder that is the copy (the data folder kept inside the install) is passed
   over, and a link into the copy, or to a folder holding it, refuses the install; a link back to a
   folder above in the install is passed over. Case is ignored in this check on every platform, so
   on a case-sensitive Linux volume a folder differing from the copy only in case is refused as
   well.
2. **Check the executable** against the manifest: 2.5.0.0's size (8,122,368 bytes) asks for the
   official 2.5.5.2 patch; 2.5.5.2's size with another hash is "modified"; any other is not the
   game.
3. **Survey** the import set: the files `analysis/results/install_manifest.tsv` lists (path, size,
   SHA-256; written by `tools/install_manifest.py`; it sits beside the hash list, wherever
   `Bundle.h` finds that), matched ignoring case, plus every `.scr` in `Assets/Scripts/` it does
   not list, imported as a mod script. A missing or empty file, or one shorter than in 2.5.5.2, is
   refused by name, all of them in one message, before anything is written. A row with no size or
   hash is unchecked: the file must be there, not empty and, for a BMP, as long as its own header
   says; its content is not compared. The install's listing goes at most 16 folders deep, every
   folder counted and links followed, and 20,000 folders in all; when it was cut short and a listed
   file was not found, the import says so rather than call the file missing.
4. **Space.** The import set plus a tenth must be free on the target's volume, or nothing is
   written.
5. **Copy** into `<game>.partial`: a clone where the file system can (APFS `clonefile`, a Linux
   reflink), a copy otherwise. A clone is hashed as read back; a copy is hashed as it is written
   (the bytes read from the install; the copy is not read back). A file whose hash differs is
   imported and listed as a warning. Ctrl-C, or the launcher's cancel, removes the partial copy;
   a second Ctrl-C ends the process at once.
6. **Move into place.** The verified copy gets `.sfcop-import.txt` (source, counts, changed files,
   mod scripts, the files of unchecked rows) and is renamed to `game`; an earlier import is moved
   aside first and removed only once the new one is in place (a run stopped between the two
   renames leaves it as `game.old`, which the next import puts back before it starts, or stops,
   leaving it, when it cannot; the report then says where it is). `install.txt` then holds
   `game`.

A run that starts from the copy accepts it as the import did: the start-up check
(`verifyInstall`) lets the files the record lists as changed or unchecked differ from the hash
list, and prints one line on each start saying how many changed files are in use and where they
are listed. Any other difference, a file changed after the import included, is refused as for
any install. Only the record in the client's own `game/` is read, and it never excuses the
executable. The start-up check covers what the hash list names: with the repository's list,
whose only runtime entry is `StarFleetOP.exe`, that is the executable alone, so an imported
copy's other files are checked once, by the import, against the manifest.

`sfcop --verify-import [--data DIR]` checks the copy again, on demand, and changes nothing; the
launcher's verify action calls the same `verifyImportedGame`. It checks the executable as the
import does, then every file of the manifest as the survey does (there, not empty, not shorter than
in 2.5.5.2, a BMP of an unchecked row as long as its header says) and hashes each checked row, with
the import's progress (phase `kVerifying`) and cancel. A file the survey refuses ends the check
there, so a changed or unreadable file is found once nothing is missing or short. Mod scripts and
the files of unchecked rows are sized, not read. The copy's own links are followed like any other
folder's, and a `game.partial` or `game.old` beside the copy is not looked at. The progress totals
leave out the executable, hashed before them by the executable's check; the report's counts include
it. The report has the import's shape: complete (exit 0) with the files whose hash differs, whether
imported so or changed since, and the mod scripts; no copy, a wrong executable, damaged files or a
copy too big to list, each named (exit 3); cancelled or no usable manifest (exit 1). A damaged copy
is mended by importing again.

`game/` is `game` in the data folder; on Windows without `--data` it is
`%LOCALAPPDATA%\SFCOP\game`, so a roaming profile does not carry the game's 750 MB. On
another machine of the same roaming profile `install.txt` says `game` but there is no copy, and
the client looks for an install as it would with no remembered one. A relative `install.txt`
value other than `game` is taken relative to the data folder.
`--install DIR` still uses DIR in place, for developers and CI. While `install.txt` says `game`
and the copy is there, such a run does not replace it, so a one-off developer run keeps the
player's link to the copy; with no copy, DIR is remembered as on any first run.

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
for one, and it writes what the client prints to `logs/launch-YYYYmmdd-HHMMSS.log` (one file per run, the newest 10 kept) in the data folder. The mission
list needs the translated modules: a double click has no environment, so name their folder once
in the data folder,

```
echo /path/to/missions/modules > ~/Library/Application\ Support/SFCOP/mission-modules.txt
```

and every later run finds them there, however it was started: the client reads the file from the
data folder it was given, not from its working directory, which the game moves to the install
while it boots. `SFCOP_MISSION_MODULES` still wins when it is set, and without either the client
looks in `mods/missions` in the data folder.
