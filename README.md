# SFCOP-decomp

![Exact](analysis/results/progress/badge-exact.svg) ![Functional](analysis/results/progress/badge-functional.svg) ![Named](analysis/results/progress/badge-named.svg)

A functional decompilation and native port of **Star Trek: Starfleet Command II — Orion Pirates**
(version 2.5.5.2) for Apple Silicon Macs, Windows and Linux, verified against the original game.

No game file ships with this project. You bring your own retail install; the port reads it in
place, unchanged.

![The main menu, running natively on macOS](media/readme/main-menu.png)

## What it is

Orion Pirates was released in 2001 for Windows and depends on layers that no longer exist: DirectX 6
interfaces, DirectPlay 4 for multiplayer, and online services for the Dynaverse campaigns that were
switched off years ago. This project rebuilds the game as readable C++ and gives it a platform
layer of its own, so the whole game runs again on current machines and its online parts run against
servers the project hosts.

- `game/` is the original game's code, recovered function by function and kept as the original
  wrote it. Every function is checked against the original binary: byte-exact where the pinned
  compiler reproduces it, behaviour-exact otherwise through a replay oracle that compares the
  native build's per-tick simulation trace with the original's.
- `src/` is the platform layer: DirectDraw, Direct3D, DirectInput, DirectPlay, Winsock, the
  registry and the sound library replaced over SDL3, the SDL GPU API, miniaudio and sockets, plus
  the fixes to the game's own bugs, each behind a switch whose default is the original behaviour.
- `server/` holds the services the online game needs: a lobby and relay for internet play between
  native clients, and the directory and authentication services the Dynaverse client logs on to.
- `oracle/`, `tools/` and `harness/` are the verification machinery: the replay oracle, the lifter
  that runs the original's code natively until it is decompiled, and the build-and-verify loop.

Until a function is decompiled, the lifter translates it from the original executable at build
time, so the native client has run the complete game from the first day while the decompiled
share grows.

## What works today

| | |
| --- | --- |
| ![Single player](media/readme/single-player.png) | ![A LAN game with two native clients](media/readme/multiplayer-lan.png) |
| Skirmish and the single-player campaigns, from the menu to combat | Multiplayer over a LAN: two native clients host and join through the game's own screens |

- The client boots to the main menu, loads the mission scripts and plays skirmishes and campaigns.
  Saves, settings and campaign files are written in the formats the original reads.
- Two native clients on one network host and join a multiplayer game.
- The Dynaverse screens reach the login: the version check, account creation and logon complete
  against this project's own directory and authentication services. The campaign server behind
  them is the next step.
- A one-command build produces a double-clickable `SFCOP.app` on macOS. Windows and Linux build
  in CI and lag in features until macOS is complete.
- Two headless runs of the oracle skirmish produce the same trace, which is what every later change
  is measured against.

![The oracle skirmish, the fixed mission the replay oracle measures every change against](media/readme/skirmish.png)

## Building and running

Requirements: a retail Orion Pirates install patched to 2.5.5.2, CMake and Ninja, a C++17
compiler, Python 3, and Docker for the lifter's toolchain.

```bash
cmake --preset macos-arm64 -DSFCOP_INSTALL_DIR="<path to your install>"
cmake --build build/macos-arm64 -j4
build/macos-arm64/sfcop --windowed
```

`sh scripts/dist.sh build/macos-arm64` writes `dist/SFCOP.app`. On first start the client looks
for the install in the usual places, asks if it cannot find it, and verifies every file it reads
against the known hashes. Its settings and saves live in the per-user data folder; the retail
install is never written to.

`sh scripts/check.sh` is the definition of done: the unit tests, the compile oracle and the replay
oracle. CI runs the same steps on the three platforms.

## How the work is organized

`AGENTS.md` is the contract for everyone and everything that commits here, `docs/DESIGN.md`
defines the behaviour, `docs/CONVENTIONS.md` the code, and `docs/PLAN.md` the work packages.
`scripts/progress.py` reports how much of the original's code is exact, functional or still
lifted, and how many functions are named; no change may lower those figures for any function.

## Progress

![Progress treemap](analysis/results/progress/treemap.svg)

Each rectangle is one translation unit of the original game, sized by its code. Bright green: every function in it is rebuilt byte-exact. Dark grey shading toward green: the share of its bytes matched so far. Library code (C runtime, STL and the like) is left out. The figures are the game-only ones in [PROGRESS.md](analysis/results/progress/PROGRESS.md):

- **Exact:** game code rebuilt byte-for-byte with the original compiler, as a share of game code bytes.
- **Functional:** exact, plus code verified to behave the same by the replay oracle.
- **Named:** game functions with a real symbol, as a share of game functions.

## Status and legal

The project is private and under active development. It contains no retail executable, library or
asset, nothing derived from one byte for byte, and nothing from any third party's source. The
licence is not yet decided; every dependency is permissively licensed. Star Trek and Starfleet
Command are trademarks of their respective owners; this is an unaffiliated preservation project.
