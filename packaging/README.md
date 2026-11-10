# packaging: the release packages

`sh scripts/dist.sh <preset>` builds one platform's client with the lift and packages it;
`.github/workflows/release.yml` runs it for the five presets on the self-hosted Mac and attaches
the packages to a draft GitHub release. Neither is part of pull-request CI.

| Preset | Runnable output in `dist/` | Release asset | Made by |
| --- | --- | --- | --- |
| `macos-arm64` | `SFCOP.app` | `SFCOP-<version>-macos-arm64.dmg` | `macos/make_bundle.sh` (the build's `sfcop_app`), `macos/make_dmg.sh` |
| `linux-x64`, `linux-arm64` | `SFCOP-<version>-linux-<arch>/` | `.tar.gz`, and `.AppImage` when it builds | `linux/make_tree.sh`, `linux/make_tarball.sh`, `linux/make_appimage.sh` |
| `windows-x64`, `windows-arm64` | `SFCOP-<version>-windows-<arch>/` | `.zip` | `windows/make_tree.sh`, `windows/make_zip.sh` |
| `source` | | `SFCOP-<version>-source.tar.gz` | `git archive` |

`<version>` is the `VERSION` file's (`SFCOP_VERSION` overrides it; the release workflow uses the
tag without its `v`).

## What each package holds

Only the client's own files, listed in `check_package.py` (`LAYOUTS`) and in
`macos/check_bundle.py` (`EXPECTED`): the client, its two SDL libraries (SDL3 and sdl2-compat,
which the lifted runtime still uses), the hash list `retail_hashes.sha256` the client verifies
the player's install against, and on Linux and Windows a `README.txt`, on Linux a launcher, a
desktop entry and the icon.

- **macOS.** `SFCOP.app` as `macos/make_bundle.sh` builds it, signed ad hoc and not notarized,
  in a compressed HFS+ image beside a link to `/Applications`.
- **Linux.** `sfcop` (a launcher that puts `lib/` on the library path), `bin/sfcop` with the hash
  list beside it, `lib/libSDL2-2.0.so.0` and `lib/libSDL3.so.0`. libstdc++ is linked statically;
  the client needs glibc 2.31 (x64, the Steam Runtime 3 sysroot) or 2.36 (arm64, a Debian 12
  sysroot, because the Steam Runtime 3 has no arm64 build) and nothing else beyond what SDL loads
  at run time (X11, GL or Vulkan, audio). `make_tree.sh` checks the client's needed libraries
  with the pinned LLVM's `llvm-readobj`. The AppImage holds the same files as an AppDir
  (`AppRun`, `usr/bin`, `usr/lib`) and is made by the pinned `appimagetool` with the pinned type 2
  runtime (`linux/appimage.env`, downloaded by `linux/fetch_appimage_tools.sh`), run in the
  linux-arm64 run image under Docker.
- **Windows.** `sfcop.exe` with `SDL2.dll`, `SDL3.dll` and the hash list beside it. The CRT is
  linked statically. Not signed.

The client finds the hash list in the bundle's `Contents/Resources` on macOS and beside its own
executable otherwise (`src/app/Bundle.h`).

## The checks

Every package is checked when it is made and again before it is attached to a release:

- `check_package.py PACKAGE HASH_LIST` mounts a `.dmg` read-only, or unpacks a `.tar.gz` or a
  `.zip` (refusing a link, an absolute path or `..` before anything is written), and refuses any
  file whose SHA-256 is in `analysis/results/retail_hashes.sha256`, a compiled mission script, a
  library that is not the client's (a translated mission module), any other file not in the
  layout, and a missing one. `--kind linux|appdir|windows DIR` checks an unpacked tree.
- `check_paths.py BUILD_DIR BINARY...` (run by `dist.sh` on the packaged binaries) refuses a
  client or library that names the checkout, the build folder, the install or the pcrecomp clone,
  and prints counts only. `dist.sh` maps those folders out at the first configure
  (`-ffile-prefix-map`) and strips debug information from the packaged copies.
  A Linux package build sets no runpath, so its client runs in place only with `LD_LIBRARY_PATH`
  naming the SDL libraries' folders, as `linux/sfcop.sh` and `linux/AppRun` set it in a package.
  A package build carries no source folder, and the test suite is not run in it
  (tests that find their fixtures beside their sources would look in the wrong place); the tests
  run in pull-request CI's ordinary builds.
- `make_appimage.sh` checks the AppDir, then unpacks the finished AppImage with its own
  `--appimage-extract` in a container of its architecture and checks what comes out.
- `release_notes.py` checks each asset once more, lists the ones that pass for upload and writes
  the draft's notes, naming every package that was not built.

`tests/test_packaging.py` tests all of these on invented trees; its `.dmg` case takes about half a
minute of `hdiutil`, so it runs only with `SFCOP_TEST_DMG=1` (the release workflow sets it).

## Mission modules

No package carries translated mission modules: they are made from the player's own `.scr` files by
`tools/scr2native`, so they are derived from the retail install, and the checks refuse them. The
client looks for them in `$SFCOP_MISSION_MODULES`, else in the folder `mission-modules.txt` in the
data folder names, else in `mods/missions/` in the data folder; the README's "Installing a
release" gives the one command a player runs.

## The release workflow

`release.yml` runs on a `v*` tag (`v` plus `VERSION`, or that with a `.` or `-` suffix for a trial draft, such as `v0.1.0-alpha.test1`) or by hand
(`gh workflow run release.yml --ref <branch> -f lift=true [-f tag=v...]`). One job on the runner
builds the five presets one after another with `scripts/dist.sh`, each step allowed to fail
without losing the others, then creates the draft release, or updates the draft of that tag in
place. A published release is never touched. The input `lift` off makes a draft that carries only
the source archive.

The runner's environment needs `SFCOP_INSTALL_DIR` and `SFCOP_PCRECOMP_DIR` (the lift's inputs)
and the five toolchains (`sh cmake/toolchains/setup.sh llvm linux windows linux-arm64
windows-arm64`). Build trees and packages go to `SFCOP_RELEASE_BUILD_ROOT`, by default
`work/release` on the external drive, because a lift build is several GB; the lift of each preset
carries over from run to run.
