# SFCOP-decomp — Design

What the system does and how it behaves. This document defines behavior; `CONVENTIONS.md` defines form; `PLAN.md` defines the order of work. Sections are numbered and stay numbered, because briefs and reviews cite them by number (`DESIGN §4.2`).

How to fill this in: one section at a time, in a design conversation with the owner. Write each behavior concretely enough that a test can be named for it. Where a decision is not made, write it in §12 rather than guessing.

---

## 1. Purpose

SFCOP-decomp rebuilds Star Trek Starfleet Command II: Orion Pirates 2.5.5.2 as readable source and ships it as a native client for macOS on Apple Silicon first, then Windows and Linux (including a SteamOS AppImage), verified against the original game's behavior. It exists so that the parts of the game that died with their services (online play, the campaign servers) can run again, and so that its networking can be modernized by a project that owns and understands the code. The one hard constraint: the repository holds no retail file or asset and nothing derived from one byte for byte. A player supplies their own installed copy, which the port verifies by hash before it reads anything.

## 2. What it does and does not do

It does:

- Run the whole game (skirmish, single-player campaign, multiplayer, Dynaverse campaign) from a retail 2.5.5.2 install on the three platforms, with the behavior the replay oracle measures on the original.
- Replace the Windows-only layers (DirectDraw 4 and Direct3D 3, DirectInput, Miles with DirectSound3D, DirectPlay 4, Winsock, the registry, SmartHeap) with a platform layer on SDL3, the SDL GPU API, miniaudio and BSD sockets.
- Play over UDP with NAT traversal and a fallback relay, with a lobby and server list from servers the owner hosts, across platforms between native clients.
- Reach a Dynaverse campaign on a self-hosted original server kit through stand-in directory servers; later, a reimplemented campaign server with the kit as its reference.
- Run mission scripts: the 88 stock missions decompiled into source over time (the SDK ships the scripting library's source and templates, not the missions), a bundled translator that turns any compiled script into a native module until then and for binary-only mods, and the 32-bit Windows build loading the originals unchanged.
- Carry the community patches' behavioral fixes, each reimplemented from its diff in this project's own source and each switchable, and fix the game's own bugs where they block play.
- Manage mods (final phase): ordered layers over the retail data, conflicts resolved by order, the retail install never touched.
- Measure itself: `scripts/progress.py` reports exact, functional and lifted code and named functions; no change may regress a function.

It does not:

- Ship, download or require anything but the player's own retail install: no assets, no models, no content from the community package.
- Change gameplay rules, balance or AI, except behind a switch whose default is the original behavior.
- Interoperate on the network with the stock Windows game; native clients are their own population.
- Reproduce Sierra's or GameSpy's services; it points the game's lookups at servers of its own.
- Target any build other than 2.5.5.2.
- Go public, or contact the community, before the owner's reveal.

## 3. Constraints

| Constraint | Value |
| --- | --- |
| Platforms | macOS 14 or later on arm64 first; Windows 10 or later x64; Linux x64 (glibc 2.35 or later) as an AppImage for SteamOS. Windows and Linux build in CI from Wave 1 and may lag in features until after the reveal |
| Languages | `game/` is C++ that compiles both with VC6 SP4 (for the byte check) and with a C++17 compiler; `src/` is C++17; tooling is Python 3 |
| Verification | A function is **exact** when its VC6 SP4 build matches the original bytes under objdiff; **functional** when it passes the replay oracle in the delinked exe and in the native build; **lifted** otherwise. Neither figure may fall for any function, compared by original address |
| Fidelity | The replay oracle's fixed skirmish produces the same per-tick trace on the native build as on the original (§5.10). Floating-point differences are a defect, not a tolerance |
| Performance | 60 frames per second at the display's native resolution on an M1 MacBook Pro, with a frame cap; the original's uncapped loop is not kept. Start to main menu in under 10 s from a warm disk |
| Data | Every original data file is read unchanged, found in the player's install and verified against `analysis/results/retail_hashes.sha256`. Saves, settings and campaign files are written in formats the original reads |
| Privacy | No telemetry. No network call without the player's action. The port stores no credential in plain text; the game's own key handling stays as the original did it |
| Licensing | Undecided (§12.1). Until decided: dependencies are permissive only (SDL3 zlib, miniaudio public domain or MIT-0); nothing derived from the WONAPI or GameSpy sources is included |
| Machine | Development and CI on one MacBook Pro; builds at `-j4`; everything large on the external drive; at most eight agents at once |
| Hosting cost | Lobby, relay and directory hosted for under 5 USD a month in total; how is decided in Phase 3 (§11) |

## 4. Domain model

The model is the original game's own, recovered from its RTTI (2,104 classes), its assertion paths and the SDK. Names below are the original identifiers and are used as-is. Three groups: the game (§4.1 to §4.6), the platform layer this project adds (§4.7), and the project's own records (§4.8).

### 4.1 Application and states

`tApplication` owns one active `tState`. The 41 states are the screens: `Startup`, `Setup` and `SelectGame`, `Skirmish`, `Campaign`, `RunCampaign`, `ContCampaign`, `EndSkirmish`, `Multiplayer`, `NetSetup`, `IPandIPX`, `GameBrowser`, `GameConnections`, `CustomizeMultiplayer`, `MetaLogon` and the Meta screens, `Tactical` (with its camera family), `VesselLibrary`, `FilmRoom`, `Academy` and `TutorialSelector`, `Medals`, `Load`, `Credits`, `Upsell`, `HotKeys`, `SupplyDock`, `Race` and `PersonalCampaignSetup`. Invariant: exactly one state is active, and every transition is one the original makes; the test is the oracle's click path reaching each screen.

### 4.2 Simulation

`tEntityMgr` owns every `tEntity`: `tEntityShip` and its `tShip*` family (`tShipSpecs`, `tShipSystems`, `tShipCommand`, `tShipFlightPath`, `tShipYard`), seekers (`tDrone*`, `tPlasma*`, `tShuttle*`), direct-fire weapons (`tPhaser*`, `tDisruptor*`, `tHeavy*`), astral objects (`tEntity_Astral_*`: suns, planets, asteroids, pulsars, wormholes, fissures), corpses and markers. Ship specs come from `shiplist.txt`, `ftrlist.txt`, `bpvdata.ini` and the per-ship model data. Invariant: the simulation is a fixed-step, order-deterministic update; given the same inputs and seed it produces the same per-tick trace on every platform (§5.10).

### 4.3 Network

Two layers. `nSwitch` (`Switch`, `SwitchAddress`, `SwitchDirectory`, `InterSwitchConnection`, `InterSwitchServer`, `CentralObjectDirectory`, `RequestWithResponse`, `AsyncProcedure`, `DataHandler`) carries Dynaverse and WON traffic over sockets. `tNetControl` and the DirectPlay 4 wrapper (`tDirectPlay*`, 0x006104D0 to 0x0061B7F0) carry game sessions: host and clients, session browsing, and the 79 `tNetMsg_k*` message classes under `tNetMsg_Header` (`kCreateShipFromSpec`, `kDamage`, `kCorrectPos`, `kChangeHost`, ...). Invariant: message semantics, framing and ordering are the original's; only the transport under them changes (§5.11).

### 4.4 Meta (Dynaverse client)

`tMetaClient*` panels (map, mission, ship, supply dock, player list, chat, news, briefing), `tMetaMap*` over `.mvm` hex maps, `tMetaMessage`, `tMetaSpecLoader`, and the database client: 171 `nStoredProcedure*` and 214 `nAsync*` procedure classes under `nDatabase`, driven by `tWonLogon` and the WON directory lookup whose hosts are in `WONServerSetup.gf`. Server profiles live in `MetaAssets/ServerProfiles`.

### 4.5 Scripting

The scripting library (`ScriptInterface`: `Script`, `State`, `Actor`, `Player`, `PlayerMgr`, `TeamInfo`, `ShipInfo`, `MissionInfo`, `EventMgr`, `VictoryCondition` and the victory states, `DrawCanvas`, `DeepScanTracker`) is exported to mission modules (`.scr`, 88 shipped plus `Campaigns/`) through `InterfaceDLLImpExp.h`. A mission module exports `MissionSpecEntry` and `ScriptEntry`. The library's source is in the SDK; the missions' is not.

### 4.6 Persistence

Settings in `.gf` files (`ClientSettings.gf`, `CommonSettings.gf`, `ServerSettings.gf`, `GameSpy.gf`, `Help.gf`, `WONServerSetup.gf`) and profiles under `MetaAssets/ClientSettings`; campaign saves; replays (`FilmRoom`, `Gen_Playback.scr`, `kCueRecording`); and the Windows registry for the key and a few options. Invariant: every file the port writes is readable by the original, and the port reads every file the original writes.

### 4.7 Platform layer (added by this project)

Window and display, GPU (the DirectDraw 4 and Direct3D 3 interfaces the game calls), input (DirectInput keyboard and mouse), audio (the Miles sample API and DirectSound3D), transport (sockets and the session transport), storage (files, the registry as a file-backed store, settings paths), memory (the system allocator in place of SmartHeap), time and threads. Each is one component with one interface the game calls through (§7).

### 4.8 Project records

A **function record**: original address, size, name and its evidence tier, status (exact, functional, lifted), owning translation unit. The **matched set**: the exact functions with their object hashes. An **oracle trace**: a mission, inputs and the per-tick hash sequence. A **fix switch**: one community or project fix, its default, and the behaviors it changes. A **mod layer**: a named set of files with an order (§5.15).

## 5. Behaviors

Each behavior names its test. "Oracle" means the replay oracle of §5.10.

### 5.1 Install discovery and verification

Input: the path to a retail 2.5.5.2 install (first run asks; later runs remember it per user). The port hashes `StarFleetOP.exe` and every file in `analysis/results/retail_hashes.sha256` and refuses to start on any mismatch, naming the file. It never writes into the install directory. Test: a fixture install with one altered byte is refused with that file named; a correct one is accepted.

### 5.2 Boot

From the install to the main menu in under 10 s on an M1 with a warm disk, without a fault, on every platform. The key prompt and its handling stay as the original does them (§12.3 for anything beyond that). Test: the boot path's oracle click sequence reaches `Setup` and the frame count at 10 s is non-zero.

### 5.3 Settings and profiles

`.gf` files and profiles are read from the install and written to a per-user data directory (`~/Library/Application Support/SFCOP` on macOS, `%APPDATA%\\SFCOP` on Windows, `$XDG_DATA_HOME/SFCOP` on Linux), in the original format. Registry keys are a file-backed store in that directory with the same names and types. Test: a `.gf` round trip is byte-identical; a registry value written by the port is read back by the game's own code path.

### 5.4 Skirmish

Set up, play and end a skirmish against AI with every race and map the original offers; medals and the end screen as the original. Test: the oracle's fixed skirmish runs to its end with the same trace as the original.

### 5.5 Single-player campaigns

The shipped campaigns (`Assets/Scripts/Campaigns`) load, save and resume; a save written by the port loads in the original and the reverse. Test: a save fixture round-trips byte-identical through the port's save path.

### 5.6 Rendering

The game's DirectDraw 4 and Direct3D 3 calls are served by the SDL GPU backend. The scene is geometrically identical to the original at the original's resolution and may be rendered at the display's native resolution with the game's own aspect handling. Windowed and fullscreen; a frame cap defaulting to the display's refresh rate; the original's uncapped loop is not kept. Pixel identity is not required; a per-frame image diff against the original's captured frames is the regression test and a change in geometry fails it.

### 5.7 Input

Keyboard and mouse through SDL3, exposed to the game through its DirectInput device interface (immediate and buffered data), with `bindings.ini` and the `HotKeys` screen as the original. Test: a scripted key sequence through the oracle driver produces the same commands as on the original.

### 5.8 Audio

The Miles sample API and the DirectSound3D buffer and listener interfaces are served by miniaudio: PCM and IMA ADPCM clips, mixing, volume and pan, looping, music streaming, and 3D positioning (position, velocity, distance limits, cones, listener frame). Test: the spike's 210 self-test checks plus a positioning check that attenuation and pan follow distance and angle.

### 5.9 Mission scripts

Three paths. (a) A decompiled mission compiles into the native build as source. (b) The bundled translator takes a compiled `.scr`, lifts it to C with the same lifter as the game, compiles it for the host, and produces a module the native build loads through the scripting library's interface; it refuses a module whose imports fall outside that interface. (c) The 32-bit Windows build loads original `.scr` files unchanged. Test: each shipped mission, through (b), drives the oracle to the same trace as the original; a module with a foreign import is refused with its name.

### 5.10 Replay oracle

A fixed skirmish mission (`oracle/scr/`) logs per-tick ship state through the scripting API; a driver runs it with scripted input and hashes the log. The trace recorded on the original is the expected value; the delinked exe and the native build must produce the same hash sequence, tick for tick, with no tolerance. Under the oracle the game's tick count and performance counter advance a fixed step per frame (16.667 ms unless `--fixed-step MS` says otherwise), never by the wall clock, its multimedia timers fall due by that clock and fire on the main thread between frames, and the original under Wine is recorded with the same step (`oracle/driver/oracle_launch.exe`). Test: `scripts/check.sh` runs the oracle on every pull request that touches `game/` or `src/`. The original's trace is recorded on Windows or another Wine build until the Mac can record it.

### 5.11 Multiplayer session

Host and join games as the original does, with the original's `tNetMsg` semantics, host migration (`kChangeHost`) and session limits. The transport under `tNetControl` is UDP with NAT traversal (hole punching through the lobby server) and a relay fallback when traversal fails; players never configure ports. Cross-platform between native clients; not compatible with the stock game. Test: two native clients on a test network with symmetric NAT complete a session; a forced traversal failure falls back to the relay within 5 s.

### 5.12 Server list and lobby

The server list, lobby and traversal rendezvous come from servers the owner hosts, whose addresses are configuration shipped with the port and editable by the player. The `GameBrowser` shows them in the original's screens. Test: an unreachable lobby shows the original's "no games" state within 10 s, not a hang.

### 5.13 Dynaverse

Phase A: the client logs on through stand-in directory and auth servers to a self-hosted original server kit, joins a campaign, moves on the hex map, takes missions and uses the supply dock as the original did. Phase B: a reimplemented campaign server speaks the same protocol, with the kit as its oracle. Test (A): a logon and one mission against the kit complete; test (B): the same client session produces the same server state on both servers.

### 5.14 Fixes

Every change to the original's behavior is a named switch with a default and a line in `docs/FIXES.md`: crash and hang fixes default on; anything a player could call a rules change defaults off. Community fixes are reimplemented from their diff, never copied. Test: each switch has a test for both positions.

No fix is written into `game/`. A fix below the Win32 surface lives in `src/` behind a runtime switch. A fix to the game's own code is an **override unit** under `src/fixes/overrides/`: a copy of the decompiled function with the change made, which the native build links in place of the original's object when the switch is on (§6.9). `game/` stays the original's source, so a reader of the decompilation never meets a switch, and a unit can be re-matched without carrying one (decided 2026-11-07).

### 5.15 Mod manager (final phase)

Mods are directories of files laid over the retail data in a player-chosen order; the manager builds the active file set, reports conflicts by order, and never changes the install. Profiles switch sets. Test: two mods touching the same file resolve to the higher layer, and disabling it restores the lower.

### 5.16 Diagnostics

Logs and crash reports are written locally only, with no identifying data, and nothing is sent anywhere. Test: a run with the network blocked logs no error.

## 6. Interfaces

The contracts two parts of the system share. The foundation wave writes each one as a file in the repository and freezes it; later packages consume them and change them only through a contract change. Paths are the ones `CONVENTIONS.md` §2 lays out.

### 6.1 The Win32 surface: what the game calls

The boundary between the game and this project is the set of Windows and DirectX entry points the game uses: 353 imports from 13 DLLs plus the COM interfaces it obtains (`IDirectDraw4`, `IDirectDrawSurface4`, `IDirectDrawClipper`, `IDirect3D3`, `IDirect3DDevice3`, `IDirect3DTexture2`, `IDirect3DViewport3`, `IDirectInputA` and `IDirectInputDeviceA`, `IDirectSound`, `IDirectSoundBuffer`, `IDirectSound3DBuffer`, `IDirectSound3DListener`, `IDirectPlay4A`, `IDirectPlayLobby3A`) and the Miles `AIL_*` C API. `src/hle/surface.tsv` lists every entry (module, name, calling convention, stack bytes purged, status, evidence) and is the contract. Status is one of `implemented`, `stub` (a fixed result), `unused` (imported, no call site reaches it) or `todo`, with the suffix `-spike` while the Phase 1 spike's module is what answers the entry; a row changes status only with the entry's implementation. It is the contract: the game side never calls outside it, and the platform side implements all of it. The game's own code is not changed to call anything else; where a port edit is unavoidable it carries a `PORT:` tag (§7.1).

### 6.2 Platform components

Each Windows subsystem is served by one component in `src/platform/` with a plain C++ interface in its header: `Window` (surface, mode, events), `Gpu` (buffers, textures, pipelines, present), `Input` (keyboard and mouse state and event queues), `Audio` (clips, voices, listener, music stream), `Transport` (IPv4 UDP and TCP sockets as the Winsock module needs them, with a `Rendezvous` sub-interface reached through the transport, which Phase 3 fills in), `Store` (files, the per-user directory, the registry-like key store), `Clock` and `Threads`. The HLE modules in `src/hle/` translate the Win32 surface onto these; the components know nothing of Win32. Tests mock a component through its header.

### 6.3 Function replacement

A decompiled function is claimed by the annotation on the line before its definition, `// FUNCTION: STARFLEETOP 0x006FC690`, exactly one per original address in the whole tree (`harness/SRC_LAYOUT.md`); `// STUB:` marks a body that compiles but is not scored. The build derives `game/manifest.tsv` from those annotations (address, size, symbol, source file) and generates the override table from it, so a claimed function's lifted body is made weak in the native build and the function's own object, compiled by VC6 and lifted (§7.2), replaces it for direct calls, the dispatch table and the guest vtables. The native build consults no table for this: the object's strong symbols win at link time over the weak exe-lift bodies, and the object's data lives in a reserved guest range (0x0F000000 to 0x10000000) that refuses writable data the exe does not already hold. In the delinked Windows build the same object replaces the original bytes. Verified status lives in `analysis/results/matched_set.json` (exact, written only by the harness) and the oracle results (functional); `scripts/progress.py` reads both. A function never appears twice in `game/`; a fix's changed copy of it lives in an override unit under `src/fixes/overrides/` (§6.9), which the harness never scores.

### 6.4 Mission modules

A mission module exports `MissionSpecEntry` and `ScriptEntry` and talks to the game only through the scripting library's headers (`ScriptInterface`). Native modules are shared libraries built with the host compiler against those headers at a pinned version and carry a version tag the loader checks; `scr2native` (§6.8) produces them from compiled `.scr` files. The 32-bit Windows build loads `.scr` files directly. A module whose imports fall outside the scripting library and the C runtime is refused.

### 6.5 Oracle trace

`oracle/expected/<mission>/<input>.trace`: one line per tick, `tick<TAB>hash`, where the hash covers the per-tick ship state the oracle mission logs, with a header naming the mission, the input script, the exe hash and the recording build. A trace from the original is the expected value; a run compares line by line and reports the first differing tick.

### 6.6 Storage and settings

The per-user data directory (§5.3) holds `settings/*.gf` and profiles in the original formats, `registry.tsv` (key, name, type, value) for the few registry values the game uses, `saves/`, `replays/`, `logs/` and `mods/`. The install directory is read-only. `src/platform/Store` is the only code that knows these paths.

### 6.7 Network protocol

The on-wire format of a game session is the dplay module's session messages, which carry the original's `tNetMsg` bundles unchanged, inside a transport envelope (`src/net/PROTOCOL.md`: version, session id, source and destination peer, sequence, reliability flags, fragment index and count). The lobby service speaks a small versioned HTTPS API for the server list, session registration and rendezvous tickets (`src/net/LOBBY_API.md`), and a UDP rendezvous for NAT traversal whose port also carries the relay (`src/net/RENDEZVOUS.md`). The relay forwards envelopes unchanged. Every message type is written down with its fields and version before any code consumes it. The Transport contract's `Rendezvous` is the client's view of it: `Bind` with the lobby's ticket, `ConnectToPeer` and `AcceptPeer` for a socket to one peer, `Unbind`.

### 6.8 Command lines

`sfcop [--install DIR] [--data DIR] [--windowed|--fullscreen] [--fix NAME=on|off] [--oracle TRACE --input SCRIPT --headless --watchdog S] [--fixed-step MS]`; `scr2native IN.scr -o OUT [--check]`; `sfcop-lobby`, `sfcop-relay` for the hosted services. Exit codes: 0 success, 2 bad input, 3 install verification failed, 4 oracle mismatch.

### 6.9 Fix switches

`docs/FIXES.md` is the list: name, default, origin (community or project), the behaviors it changes, its tests. A switch is one of two kinds. A `build` switch selects an override unit: `src/fixes/overrides/<Dir>/<unit>.cpp` holds the changed copy of one or more functions, each annotated `// OVERRIDE: STARFLEETOP 0x... fix_name` on the line before it, in the same VC6 C++ as `game/` and compiled and lifted the same way (§7.2); when the switch is on, the native build links the override's object in place of the `game/` object (or the lifted body) for those functions, and when it is off the original is linked. The byte check and the 32-bit build never see an override, so a fix never changes a function's status. The switch takes its default or a `-D` value in the native build; `SFCOP_FIX(name)` from `src/fixes/fixes.h` may appear only under `src/fixes/overrides/`, never in `game/`, and `scripts/check.sh` fails on a site there. `tools/manifest.py` lists the override units beside the manifest; that listing and the link choice are built with the first rules change (PLAN §6.6, F2). An override unit is not the guest override table of §6.3 (`src/app/guest/override.*`), which routes every decompiled function to its object whatever the switches say. A `runtime` switch is read in `src/` through `sfcop::fixes::isEnabled`, from the default, then `settings/fixes.gf`, then `--fix name=on|off`, each later source winning. The switches are declared in `src/fixes/switches.def`, in `docs/FIXES.md` order, so adding a fix changes no frozen header.

### 6.10 Progress and matched-set files

`analysis/results/progress/progress.json` and `PROGRESS.md` (written only by `scripts/progress.py`) and `analysis/results/matched_set.json` (written only by the harness) keep the shapes P1-5 and P1-6 fixed. The gate compares against the base branch's copy.

## 7. Architecture

### 7.1 Layers

```
game/        the original's code: lifted bodies (generated, never committed) and decompiled source
             calls only the Win32 surface of §6.1
src/hle/     the Win32 surface implemented on the platform components
src/platform the components of §6.2 on SDL3, SDL GPU, miniaudio, sockets, the C++ runtime
src/net/     transport, rendezvous, relay client (Phase 3)
src/fixes/   fix switches, and the override units that carry fixes to the game's own code
third_party/ pinned SDL3, miniaudio; nothing derived from WONAPI or GameSpy
tools/       scr2native, the lifter driver, build helpers
server/      lobby, relay and directory stand-ins (Phase 3 and 4)
```

**The decompilation and the modernization are kept apart so that the decompilation can be forked on its own.** `game/`, `harness/`, `oracle/`, `toolchain/`, `analysis/`, `ghidra/` and `tools/` are the decompilation: they aim at the original's behavior and nothing else, and a build of `game/` with the HLE surface answered by the original DLLs is the original game: `game/` carries no fix. `src/`, `server/` and the mod manager are the modernization: they live below the Win32 surface or in override units behind fix switches, and nothing in `game/` depends on them. A fork that wants only the faithful decompilation takes the first group and ignores the second; `scripts/check.sh` counts and lists the `PORT:` lines in `game/`, fails on any `SFCOP_FIX` site there, and counts the override units, so the size of the modernization's footprint inside the decompilation is always visible. Splitting into two repositories later is a mechanical move because of this boundary; it is not done now because one repository is one runner, one review pipeline and one set of pull requests (§12.11).

Rules. `game/` includes nothing from `src/` and carries no fix switch (a fix to the game's code is an override unit, §6.9); a port-motivated edit inside `game/` is tagged `PORT:` on its line with the reason, and there are as few as the platform allows (64-bit and endianness hygiene, undefined behavior the original relied on). `src/hle/` includes `game/` headers only for the struct layouts the surface needs. `src/platform/` includes no Win32 header on any platform. `third_party/` is changed only by the integrator.

### 7.2 Two builds, two compile modes

The **32-bit Windows build** is the delinked original relinked with VC6 SP4 in the toolchain container: decompiled functions replace the original's bytes one by one, checked by objdiff and the oracle. It runs on Windows and under Wine and is the test bed and the Windows deliverable until the native build overtakes it.

The **native build** is the lifted translation of the exe compiled with clang for arm64 and x64, plus the platform layer, with decompiled functions dropped in from the manifest. Lifted code runs in a 32-bit guest address space inside the 64-bit process: guest pointers are 32-bit offsets from a fixed base, structures keep their original layouts, and virtual calls go through the guest vtables. A decompiled function enters the native build the way the rest of the game does: its unit is compiled by VC6 SP4 in the toolchain container, exactly as the harness compiles it for the byte check, and the resulting COFF object is lifted, its relocations resolved against the exe's symbols, and linked in place of the function's lifted body (§6.3). `game/` is therefore plain VC6 C++ with no guest-mode macros, and the object the native build runs is the one the harness scored (decided 2026-11-04, §12.4). The alternative, a second compile of `game/` by clang in a guest mode through `game/port/types.h`, was proven on three functions by the C3 spike (`spike/guest-mode/REPORT.md`). It is retired: the spike's build no longer compiles against `game/` as written since (plain VC6 types, ordinary pure virtuals), its header stays in `game/port/` only as the spike's record, and nothing in `game/` depends on it.

When the decompilation is complete the guest mode is retired: the whole game compiles as ordinary 64-bit C++, and the lifter and the VC6 container leave the native build.

### 7.3 Processes and threads

One client process. The game's own threads (worker events, the sound service) stay as they are; the platform runs SDL's event loop on the main thread and miniaudio on its own thread. Hosted services are separate small processes (`sfcop-lobby`, `sfcop-relay`) with no shared state beyond their own stores. The Dynaverse server kit runs as its own process under Wine on a Linux host.

### 7.4 Build and verification graph

Developer machines and the runner have the retail install; CI generates the lifted bodies from it into the build directory (never into git), compiles the claimed units of `game/` with VC6 in the toolchain container and lifts their objects, and builds both targets. A unit whose source, headers, tools and exe-lift inputs are unchanged recompiles and re-lifts nothing; a machine without the install, pcrecomp or the toolchain container (the CI runner today) builds nothing lifted and runs the rest of `scripts/check.sh`. `scripts/check.sh` runs the Python tests, the hotspot check, the progress gate and the matched-set gate against the base branch, and the oracle for changes under `game/` or `src/`. A release of the native client is produced only by the owner (§12.2).

### 7.5 Empires at War, a later second module

Orion Pirates expands Star Trek Starfleet Command II: Empires at War (retail exe `StarFleet2.exe`, 2000). Decompiling it is a later project in this repository, as a second module beside `STARFLEETOP`. Until it starts, Orion Pirates 2.5.5.2 is the only target: no build, test, fixture, oracle trace or CI step uses the Empires at War exe, and no annotation names another module (`tools/manifest.py` refuses one).

A comparison of the two exes (2026-11-08, by a script in the grind integrator's task directory `work/eaw-overlap/`, not committed; both exes carry the same compiler build in their Rich headers) measured Orion Pirates' game code, library code excluded, by bytes: 31% identical in Empires at War apart from addresses, 4% identical apart from structure offsets, 17% with small edits, 48% with no counterpart. The overlap is concentrated:

| Area | Identical | Edited or offsets only | No counterpart |
| --- | --- | --- | --- |
| `Tiger/` | 81% | 10% | 9% |
| `dmp3d/` | 42% | 29% | 29% |
| The game proper (`Source/`, most of `unsorted/`) | 27% | 25% | 49% |
| `Common/`, `Switch/`, `Meta/`, `Source/MetaClient/` | 0 to 3% | 0 to 8% | 90 to 99% |

The identical figure is a lower bound: the comparison found Empires at War's function starts by heuristic. Both games' assertion paths agree with the split: the two libraries came from a project of their own in both builds, and each game's code from its own project folder.

What follows now:

- The grind decompiles shared code first (PLAN §6.3 row D). Because `game/` holds the original's code and nothing else (§7.1), every function it matches in `Tiger/`, `dmp3d/` and the shared game code is work already done for the second module.
- Code outside `game/` names decompiled functions by original address (the manifest, the override table, override units), never by a path in `game/`. A pull request that moves files in `game/` updates every `#include` of them in the same pull request.
- Feature work in `src/` and `server/` is for Orion Pirates; nothing in it waits for or serves the second module.

What happens when the second module starts (§12.12): `Tiger/` and `dmp3d/` move to a tree shared by both modules, with one annotation per binary on each function they share (the harness then reads a stack of annotations above a function, and the manifest and the lifter key functions by module and address); each game's own code gets a tree of its own, because Orion Pirates' project is a fork of Empires at War's with about half its code changed; a function that changed between the two games has one copy per game.

## 8. Data and storage

| State | Where | Identified by | Changes | Backup |
| --- | --- | --- | --- | --- |
| The retail install | Where the player put it | The hash list `analysis/results/retail_hashes.sha256` | Never by this project | The player's own media |
| Settings, profiles, registry values, saves, replays, logs, mods | The per-user data directory of §5.3 and §6.6 | The player's OS account | Written atomically (write to a temporary file, rename); formats the original reads | The directory is plain files the player can copy; no cloud |
| Decompiled source, manifest, matched set, expected traces | The repository | Original address and symbol | Pull requests only; the gates refuse regressions | git |
| Lifted code, Ghidra project, oracle prefixes, Docker images | `work/` on the external drive | Exe hash and tool version in each output's header | Regenerated by scripts; nothing there is a source of truth | None needed; everything regenerates from the install |
| Lobby and server list | The hosted lobby's store (one SQLite file) | Session id, server name | Registrations expire after 2 minutes without a heartbeat | Nothing worth keeping; a restart starts empty |
| Relay | None; it forwards | Session id | | |
| Dynaverse campaign | The server kit's flat files on its host | The kit's own ids | By the kit | Nightly copy of the kit's data directory on the host |

## 9. Security and privacy

- **Who can do what.** The client is a single-player program with no accounts. The hosted lobby accepts registrations and lookups from any client speaking the current protocol version; it rate-limits by address and the owner can block an address. The relay forwards only between machines of one session that each hold a ticket the lobby issued for it. Stand-in directory and auth services do not reproduce Sierra's account system: a player is a name they type plus an id generated on their machine; the owner decides anything beyond that (§12.3).
- **Secrets.** The hosted services' credentials (TLS certificates, the owner's admin token) live in environment files outside git on the host, or in GitHub repository secrets. The repository holds none, and no test fixture holds a real one.
- **Never logged.** CD keys, the contents of the key store, player addresses beyond what the relay and lobby need to run (kept 7 days, then deleted), and anything from the player's install. Logs on the player's machine stay there (§5.16).
- **Never committed.** Retail files or bytes, lifted code, the owner's private files, personal data, anything derived from the WONAPI or GameSpy sources. The hotspot check and the review read for these.
- **Input from the network and from mods is untrusted.** Every message is bounds-checked against its written format before the game sees it; the original did not, and the fixes that add the checks default on (§5.14). A mission module is loaded only after its import list passes §6.4.

## 10. Operations

- **Development.** One MacBook Pro with the install, Docker (the VC6 container), Ghidra and Wine; everything large under `work/` on the external drive; up to eight agents at once at `-j4`.
- **CI.** The self-hosted runner `sfcop-mac` on the same machine runs `check`, `hotspots` and the review on every pull request; nothing runs on GitHub-hosted runners. Runbook: if the runner is offline, `cd ~/gitdir/actions-runner-sfcop && ./svc.sh stop && ./svc.sh start`; its logs are in `_diag/` and `~/Library/Logs/actions.runner.*`. The Windows and Linux builds cross-compile on the runner from Wave 1; a real Windows or Linux machine is added when one is available.
- **Hosted services (Phase 3 and 4).** One small Linux VPS runs `sfcop-lobby`, `sfcop-relay`, the directory and auth stand-ins and, for Phase 4, the server kit under Wine. Deployed by a script from the repository, with a health endpoint per service checked every minute by the host's own monitor; a failure sends the owner one message. Logs rotate at 7 days.
- **Releases.** None until the decompilation is complete (§12.2). Until then the owner builds locally; a release later is a tagged `main` with the source and the one-command build tool, and possibly a signed launcher from the owner's developer account.
- **Recording the oracle.** On a Windows machine or another Wine build, with `oracle/README.md`; the recorded trace is committed under `oracle/expected/` with its header, and re-recorded only when the mission or input script changes.

## 11. Cost

| Item | Monthly | Notes |
| --- | --- | --- |
| Agents | 0 USD beyond the owner's existing Claude Max plan | Token spend is bounded by the plan, not billed per token; `PROPOSAL.md` §6 has the effort estimate |
| CI | 0 USD | Self-hosted runner; no Actions minutes |
| Hosted services | Under 5 USD from Phase 3, the owner's ceiling | Lobby, relay, stand-in directory and, later, the server kit; the cheapest shape (one small VPS, a free tier, or a machine the owner already runs) is chosen in Phase 3; a domain is about 1 USD a month |
| Signing and notarization | 0 USD until the project is complete | The owner's existing developer account signs the launcher if one ships at the end (§12.2) |
| Storage | 0 USD | The external drive already owned |

Expected total: 0 USD a month until Phase 3, then under 5 USD.

## 12. Open questions

| # | Question | Who decides | Blocks | Status |
| --- | --- | --- | --- | --- |
| 12.1 | The licence of this project's own code, and therefore whether a `LICENSE` file exists | Owner | The reveal; nothing before it | Open |
| 12.2 | Distribution of the native binary while it still carries translated code | Owner | Release packaging | **Decided 2026-11-03:** no binary leaves the owner until the decompilation is complete; if an earlier release happens it is source plus a one-command build tool, kept simple. Once complete, the owner may ship a signed launcher built with their developer account |
| 12.3 | What the native client does at the key prompt and with the game's online key check once the original services are gone | Owner | Phase 3 lobby design | Open; until decided the native client behaves as the original |
| 12.4 | Whether `game/` source compiles in both the VC6 mode and the guest mode with `game/port/types.h` alone (§7.2) | The C3 spike, then the owner | Replacing lifted functions in the native build; the "one source tree" claim | **Decided 2026-11-04:** the spike answered yes with constraints (`spike/guest-mode/REPORT.md`); the owner chose the alternative instead: each decompiled unit is compiled by VC6 and its object lifted into the native build (§7.2), so `game/` carries no guest-mode constructs. The object lifter landed as Wave 1 package B9 (#39); the guest-mode route is retired as a build and kept as the spike's record |
| 12.5 | Whether the scripting library's source files, which the SDK ships to modders, may be committed, or must be copied from the player's install at build time | Owner | Script packages; the translator | Open; the build copies from the install until decided |
| 12.6 | Which transport Dynaverse missions use: the DirectPlay wrapper or `nSwitch` | Analysis (`analysis/netcode/`) in Phase 3 | The Phase 4 transport design | **Answered 2026-11-07 (N1, `analysis/netcode/dynaverse.md`):** the DirectPlay wrapper through `tNetControl`. `tState_RunCampaign` hosts or joins over TCP/IP, by connect-by-address to the host address in the server's `tMatchedMission`. nSwitch carries the matching and the results; during the mission the client has it cache Dynaverse messages, and (inferred, from static analysis) carries no combat traffic. A Phase 4 server writes the host's rendezvous name into that field |
| 12.7 | Whether the lifted build's 53-bit floating point reproduces the original's traces | The oracle, once a trace exists | Any "functional" claim for simulation code in the native build | Open |
| 12.8 | NAT traversal: libjuice, GameNetworkingSockets, or the project's own punch over Transport | Integrator, in the Phase 3 protocol document | Phase 3 transport package | **Decided 2026-11-07:** the project's own punch over Transport, with its own relay (`src/net/RENDEZVOUS.md`, `NAT_LIBRARY.md`); no library. libjuice (pinned in `NAT_LIBRARY.md`) is the fallback if the punch fails N2's acceptance, taken by a contract change |
| 12.9 | Which machine records the oracle (a Windows PC, CrossOver, another Wine build) | Owner | Every "functional" figure | Open |
| 12.10 | Which community fixes default on and which are rules changes | Owner, reviewing `docs/FIXES.md` as it fills | Nothing until the fixes packages | Open |
| 12.11 | Whether the decompilation and the modernization split into two repositories | Owner, at the reveal | Nothing before it | **Decided 2026-11-03:** one repository with the boundary of §7.1 enforced by the layout and the check; revisit at the reveal |
| 12.12 | Empires at War as a second module (§7.5): which build is the target (the 2000 retail exe or a later official patch), and when `Tiger/` and `dmp3d/` move to the shared tree | Owner, when that work starts | The second module; nothing before it | Open; Orion Pirates is the only target until then |
