# SFCOP-port — Design

> **Scope.** This repository is the port: the platform layer, the fixes, the servers, the lifter and the replay oracle. The decompiled game code lives in SFC2-decomp, pinned here as the submodule `decomp/`; what this document says of it is only what the port relies on, and SFC2-decomp's own documents govern that code. `decomp/docs/INTERFACE.md` is the contract between the two.

What the system does and how it behaves. This document defines behavior; `CONVENTIONS.md` defines form; `PLAN.md` defines the order of work. Sections are numbered and stay numbered, because briefs and reviews cite them by number (`DESIGN §4.2`).

How to fill this in: one section at a time, in a design conversation with the owner. Write each behavior concretely enough that a test can be named for it. Where a decision is not made, write it in §12 rather than guessing.

---

## 1. Purpose

SFCOP-port ships Star Trek Starfleet Command II: Orion Pirates 2.5.5.2, rebuilt as readable source by the decompilation (SFC2-decomp), as a native client for macOS on Apple Silicon first, then Windows and Linux (including a SteamOS AppImage), verified against the original game's behavior. It exists so that the parts of the game that died with their services (online play, the campaign servers) can run again, and so that its networking can be modernized by a project that owns and understands the code. The one hard constraint: the repository holds no retail file or asset and nothing derived from one byte for byte. A player supplies their own installed copy, which the port verifies by hash before it reads anything.

## 2. What it does and does not do

It does:

- Run the whole game (skirmish, single-player campaign, multiplayer, Dynaverse campaign) from a retail 2.5.5.2 install on the three platforms, with the behavior the replay oracle measures on the original.
- Replace the Windows-only layers (DirectDraw 4 and Direct3D 3, DirectInput, Miles with DirectSound3D, DirectPlay 4, Winsock, the registry, SmartHeap) with a platform layer on SDL3, the SDL GPU API, miniaudio and BSD sockets.
- Play over UDP with NAT traversal and a fallback relay, with a lobby and server list from servers the owner hosts, across platforms between native clients.
- Reach a Dynaverse campaign on a self-hosted original server kit through stand-in directory servers; later, a reimplemented campaign server with the kit as its reference.
- Run mission scripts: the 88 stock missions decompiled into source over time (the SDK ships the scripting library's source and templates, not the missions), a bundled translator that turns any compiled script into a native module until then and for binary-only mods, and the 32-bit Windows build loading the originals unchanged.
- Carry the community patches' behavioral fixes, each reimplemented from its diff in this project's own source and each switchable, and fix the game's own bugs where they block play.
- Manage mods (final phase): ordered layers over the retail data, conflicts resolved by order, the retail install never touched.
- Measure itself: the decompilation reports exact, functional and lifted code and named functions in its own repository; the port takes only functions it records exact, and the replay oracle here measures behaviour.

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
| Languages | `src/`, `server/` and `tools/` are C++17; tooling is Python 3; the decompiled game is the decompilation's VC6 C++ (`decomp/op/`), compiled here only by its pinned VC6 |
| Verification | Whether a function is **exact** is the decompilation's record (`decomp/op/manifest.tsv` modes, `decomp/docs/INTERFACE.md`); here a function is **functional** when it passes the replay oracle in the native build, and **lifted** otherwise |
| Fidelity | The replay oracle's fixed skirmish produces the same per-tick trace on the native build as on the original (§5.10). Floating-point differences are a defect, not a tolerance |
| Performance | 60 frames per second at the display's native resolution on an M1 MacBook Pro, with a frame cap; the original's uncapped loop is not kept. Start to main menu in under 10 s from a warm disk |
| Data | Every original data file is read unchanged, found in the player's install and verified against `decomp/analysis/results/retail_hashes.sha256`. Saves, settings and campaign files are written in formats the original reads |
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

Input: the path to a retail 2.5.5.2 install (first run asks; later runs remember it per user). The port hashes `StarFleetOP.exe` and every file in `decomp/analysis/results/retail_hashes.sha256` and refuses to start on any mismatch, naming the file. It never writes into the install directory. Test: a fixture install with one altered byte is refused with that file named; a correct one is accepted.

### 5.2 Boot

From the install to the main menu in under 10 s on an M1 with a warm disk, without a fault, on every platform. The key prompt and its handling stay as the original does them (§12.3 for anything beyond that). Test: the boot path's oracle click sequence reaches `Setup` and the frame count at 10 s is non-zero. Budget, as measured on the lifted client (S1, #81): the original's own splash and loading screens take about 4.5 s and cannot be shortened without changing the game; the port's share (mapping the exe, binding imports, opening the mission modules ahead of use) is about 3.4 s, and that is where any further gain comes from. The rest, up to about 1.3 s, is the host's own start (mapping the executable and binding its imports) and run-to-run variance; Select Game at 8.4 to 9.2 s on an M1 Pro with the machine otherwise idle, inside the 10 s gate above.

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

A fixed skirmish mission (`oracle/scr/`) logs per-tick ship state through the scripting API; a driver runs it with scripted input and hashes the log. The trace recorded on the original is the expected value; the delinked exe and the native build must produce the same hash sequence, tick for tick, with no tolerance. Under the oracle the game's tick count and performance counter advance a fixed step per frame (16.667 ms unless `--fixed-step MS` says otherwise), never by the wall clock, its multimedia timers fall due by that clock and fire on the main thread between frames, and the original under Wine is recorded with the same step (`oracle/driver/oracle_launch.exe`). Test: CI runs the oracle on every pull request that touches the `decomp` pin or `src/`. The original's trace is recorded on Windows or another Wine build until the Mac can record it.

### 5.11 Multiplayer session

Host and join games as the original does, with the original's `tNetMsg` semantics, host migration (`kChangeHost`) and session limits. The transport under `tNetControl` is UDP with NAT traversal (hole punching through the lobby server) and a relay fallback when traversal fails; players never configure ports. Cross-platform between native clients; not compatible with the stock game. The DirectPlay module lists a modem service provider as the original's DirectPlay did, because the game reads connection kind 3 (modem) without checking that the provider exists (`STARFLEETOP 0x00616700`); it is never offered to the player and never connects. Test: two native clients on a test network with symmetric NAT complete a session; a forced traversal failure falls back to the relay within 5 s.

### 5.12 Server list and lobby

The server list, lobby and traversal rendezvous come from servers the owner hosts, whose addresses are configuration shipped with the port and editable by the player. The `GameBrowser` shows them in the original's screens. Test: an unreachable lobby shows the original's "no games" state within 10 s, not a hang.

### 5.13 Dynaverse

Phase A: the client logs on through stand-in directory and auth servers to a self-hosted original server kit, joins a campaign, moves on the hex map, takes missions and uses the supply dock as the original did. Phase B: a reimplemented campaign server speaks the same protocol, with the kit as its oracle. Test (A): a logon and one mission against the kit complete; test (B): the same client session produces the same server state on both servers.

### 5.14 Fixes

Every change to the original's behavior is a named switch with a default and a line in `docs/FIXES.md`: crash and hang fixes default on; anything a player could call a rules change defaults off. Community fixes are reimplemented from their diff, never copied. Test: each switch has a test for both positions.

No fix is written into the decompilation (`decomp/op/`). A fix below the Win32 surface lives in `src/` behind a runtime switch. A fix to the game's own code is an **override unit** under `src/fixes/overrides/`: a copy of the decompiled function with the change made, which the native build links in place of the original's object when the switch is on (§6.9). `decomp/op/` stays the original's source, so a reader of the decompilation never meets a switch, and a unit can be re-matched without carrying one (decided 2026-11-07).

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

The decompilation claims functions in `decomp/op/manifest.tsv` (address, size, symbol, source, modes; `decomp/docs/INTERFACE.md`). For each row whose modes include `vc6` (byte-exact), the native build compiles the unit with the decompilation's pinned VC6 (`decomp/toolchain/`), lifts the object (`tools/objlift/`) and links it in place of the function's lifted body, for direct calls, the dispatch table and the guest vtables (§7.2); a `partial` or `stub` row keeps the original's lifted body. The guest table `src/app/guest/override.cpp` is generated from the same annotations by the decompilation's `tools/manifest.py`. A fix's changed copy of a function lives in an override unit under `src/fixes/overrides/` (§6.9), never in the decompilation.

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

`docs/FIXES.md` is the list: name, default, origin (community or project), the behaviors it changes, its tests. A switch is one of two kinds. A `build` switch selects an override unit: `src/fixes/overrides/<Dir>/<unit>.cpp` holds the changed copy of one or more functions, each annotated `// OVERRIDE: STARFLEETOP 0x... fix_name` on the line before it, in the same VC6 C++ as `decomp/op/` and compiled and lifted the same way (§7.2); when the switch is on, the native build links the override's object in place of the decompilation's object (or the lifted body) for those functions, and when it is off the original is linked. The byte check and the 32-bit build never see an override, so a fix never changes a function's status. The switch takes its default or a `-D` value in the native build; `SFCOP_FIX(name)` from `src/fixes/fixes.h` may appear only under `src/fixes/overrides/`, never in `decomp/op/`; the decompilation's own check fails on a site there. Which tool scans `// OVERRIDE:` annotations and lists the override units, now that `tools/manifest.py` belongs to the decompilation, is an open decision to take before F2 builds that listing and the link choice (PLAN §6.6). An override unit is not the guest override table of §6.3 (`src/app/guest/override.*`), which routes every decompiled function to its object whatever the switches say. A `runtime` switch is read in `src/` through `sfcop::fixes::isEnabled`, from the default, then `settings/fixes.gf`, then `--fix name=on|off`, each later source winning. The switches are declared in `src/fixes/switches.def`, in `docs/FIXES.md` order, so adding a fix changes no frozen header.

### 6.10 Progress and matched-set files

The progress and matched-set files belong to the decompilation; the port reads only what `decomp/docs/INTERFACE.md` lists of them.

## 7. Architecture

### 7.1 Layers

```
decomp/op/   the original's code, decompiled (the submodule); with the lifted bodies (generated,
             never committed) it calls only the Win32 surface of §6.1
src/hle/     the Win32 surface implemented on the platform components
src/platform the components of §6.2 on SDL3, SDL GPU, miniaudio, sockets, the C++ runtime
src/net/     transport, rendezvous, relay client (Phase 3)
src/fixes/   fix switches, and the override units that carry fixes to the game's own code
third_party/ pinned SDL3, miniaudio; nothing derived from WONAPI or GameSpy
tools/       scr2native, the lifter driver, build helpers
server/      lobby, relay and directory stand-ins (Phase 3 and 4)
```

**The decompilation is its own repository** (SFC2-decomp, split out on 2026-11-16, §12.11), pinned here as `decomp/` and read only through `decomp/docs/INTERFACE.md`. It aims at the original's behavior and nothing else; `src/`, `server/` and the mod manager are the modernization, below the Win32 surface or in override units behind fix switches, and nothing in `decomp/op/` depends on them.

Rules. `decomp/op/` includes nothing from `src/` and carries no fix switch (a fix to the game's code is an override unit, §6.9); a port-motivated edit there is a `PORT:` line made in the decompilation's repository, never here. `src/hle/` includes `decomp/op/` headers only for the struct layouts the surface needs. `src/platform/` includes no Win32 header on any platform. `third_party/` is changed only by the integrator.

### 7.2 Two builds, two compile modes

The **32-bit Windows build** is the delinked original relinked with VC6 SP4, with decompiled functions replacing the original's bytes; it is the decompilation's test bed and lives in its repository, not here.

The **native build** is the lifted translation of the exe compiled with clang for arm64 and x64, plus the platform layer, with decompiled functions dropped in from the manifest. Lifted code runs in a 32-bit guest address space inside the 64-bit process: guest pointers are 32-bit offsets from a fixed base, structures keep their original layouts, and virtual calls go through the guest vtables. A decompiled function enters the native build the way the rest of the game does: its unit is compiled by VC6 SP4 in the toolchain container, exactly as the decompilation compiles it for its byte check, and the resulting COFF object is lifted, its relocations resolved against the exe's symbols, and linked in place of the function's lifted body (§6.3). The decompiled source is therefore plain VC6 C++ with no guest-mode macros, and the object the native build runs is the one the decompilation scored (decided 2026-11-04, §12.4). The alternative, a second compile by clang in a guest mode through `decomp/op/port/types.h`, was proven on three functions by the C3 spike (`spike/guest-mode/REPORT.md`) and is retired; that header stays only as the spike's record.

When the decompilation is complete the guest mode is retired: the whole game compiles as ordinary 64-bit C++, and the lifter and the VC6 container leave the native build.

### 7.3 Processes and threads

One client process. The game's own threads (worker events, the sound service) stay as they are; the platform runs SDL's event loop on the main thread and miniaudio on its own thread. Hosted services are separate small processes (`sfcop-lobby`, `sfcop-relay`) with no shared state beyond their own stores. The Dynaverse server kit runs as its own process under Wine on a Linux host.

### 7.4 Build and verification graph

Developer machines and the runner have the retail install; the build generates the lifted bodies from it into the build directory (never into git), compiles the units of `decomp/op/` that have `vc6` rows with the decompilation's VC6 container and lifts their objects (`tools/objlift/`), and links the native client. Inputs that did not change are not recompiled or re-lifted; a machine without the install, pcrecomp or the container builds nothing lifted. `scripts/check.sh` runs the Python tests, checks that `.github/hotspots.txt` and `CODEOWNERS` agree, checks that `src/app/guest/override.cpp` is what the pinned decompilation generates, runs the C++ tests (CTest) when the native build is configured, and runs the replay oracle when a recorded trace, the client and a prepared data folder exist. A release of the native client is built by the release workflow from a tag and published only by the owner (§12.2).

### 7.5 Empires at War, a later second module

Empires at War is the decompilation's second module; the port never reads `decomp/eaw/`, and nothing in `src/` or `server/` waits for or serves it.

## 8. Data and storage

| State | Where | Identified by | Changes | Backup |
| --- | --- | --- | --- | --- |
| The retail install | Where the player put it | The hash list `decomp/analysis/results/retail_hashes.sha256` | Never by this project | The player's own media |
| Settings, profiles, registry values, saves, replays, logs, mods | The per-user data directory of §5.3 and §6.6 | The player's OS account | Written atomically (write to a temporary file, rename); formats the original reads | The directory is plain files the player can copy; no cloud |
| Decompiled source, manifest, matched set | The decompilation (SFC2-decomp), through the `decomp` pin | Original address and symbol | Pull requests there; a bump here moves the pin | git |
| Expected traces, the generated override table | This repository | Mission and input; original address | Pull requests only | git |
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

- **Development.** One MacBook Pro with the install, Docker (the VC6 container) and Wine (Ghidra is the decompilation's, not needed for port work); everything large under `work/` on the external drive; up to eight agents at once at `-j4`.
- **CI.** The self-hosted runner `sfcop-mac` on the same machine runs `check`, the builds and the oracle; `plan`, `hotspots` and the review run on GitHub-hosted `ubuntu-latest`. Runbook: if the runner is offline, `cd ~/gitdir/actions-runner-sfcop && ./svc.sh stop && ./svc.sh start`; its logs are in `_diag/` and `~/Library/Logs/actions.runner.*`. The Windows and Linux builds cross-compile on the runner from Wave 1; a real Windows or Linux machine is added when one is available.
- **Hosted services (Phase 3 and 4).** One small Linux VPS runs `sfcop-lobby`, `sfcop-relay`, the directory and auth stand-ins and, for Phase 4, the server kit under Wine. Deployed by a script from the repository, with a health endpoint per service checked every minute by the host's own monitor; a failure sends the owner one message. Logs rotate at 7 days.
- **Releases.** A tag `v<VERSION>` on `main` (the `VERSION` file, with an optional pre-release suffix) runs `.github/workflows/release.yml` on the runner, which builds the five packages (macOS arm64 disk image, Linux x64 and arm64 archives, Windows x64 and arm64 archives) with `SFCOP_PACKAGE_BUILD=ON`, checks each with `packaging/check_package.py` and `packaging/check_paths.py`, and attaches them to a draft release that only the owner publishes (§12.2). The tag is created by the owner, or by an agent only when the owner says so (`AGENTS.md`, "Default autonomy and safety"). The workflow is not part of pull request CI. Unsigned until the project is complete; a signed launcher from the owner's developer account is still the plan for the end.
- **Recording the oracle.** On a Windows machine or another Wine build, with `oracle/README.md`; the recorded trace is committed under `oracle/expected/` with its header, and re-recorded only when the mission or input script changes.

## 11. Cost

| Item | Monthly | Notes |
| --- | --- | --- |
| Agents | 0 USD beyond the owner's existing Claude Max plan | Token spend is bounded by the plan, not billed per token; `docs/history/PROPOSAL.md` §6 has the effort estimate |
| CI | 0 USD | Self-hosted runner; no Actions minutes |
| Hosted services | Under 5 USD from Phase 3, the owner's ceiling | Lobby, relay, stand-in directory and, later, the server kit; the cheapest shape (one small VPS, a free tier, or a machine the owner already runs) is chosen in Phase 3; a domain is about 1 USD a month |
| Signing and notarization | 0 USD until the project is complete | Alpha packages are unsigned (§12.2); the owner's existing developer account signs the launcher if one ships at the end |
| Storage | 0 USD | The external drive already owned |

Expected total: 0 USD a month until Phase 3, then under 5 USD.

## 12. Open questions

| # | Question | Who decides | Blocks | Status |
| --- | --- | --- | --- | --- |
| 12.1 | The licence of this project's own code, and therefore whether a `LICENSE` file exists | Owner | The reveal; nothing before it | Open |
| 12.2 | Distribution of the native binary while it still carries translated code | Owner | Release packaging | **Decided 2026-11-09 (supersedes 2026-11-03):** a 0.1-alpha of the native client may be published while it still carries translated code. Each package is the native client; its game code is translated from the original executable when the release is built on the runner (`.github/workflows/release.yml`, `packaging/`), and it carries no file of the original game: `packaging/check_package.py` refuses a package holding any file of the hash list, and `packaging/check_paths.py` refuses one that names a build or install folder. The player's own install is used only at run time, for data and the hash check (§5.1). The lift on is the release default; a release built with the lift off is source only, the fallback. Signing and notarization are out of scope for the alpha. The 2026-11-03 position (no binary until the decompilation is complete) is withdrawn |
| 12.3 | What the native client does at the key prompt and with the game's online key check once the original services are gone | Owner | Phase 3 lobby design | Open; until decided the native client behaves as the original |
| 12.4 | Whether the decompiled source (then `game/`) compiles in both the VC6 mode and the guest mode with `game/port/types.h` alone (§7.2) | The C3 spike, then the owner | Replacing lifted functions in the native build; the "one source tree" claim | **Decided 2026-11-04:** the spike answered yes with constraints (`spike/guest-mode/REPORT.md`); the owner chose the alternative instead: each decompiled unit is compiled by VC6 and its object lifted into the native build (§7.2), so `game/` carries no guest-mode constructs. The object lifter landed as Wave 1 package B9 (#39); the guest-mode route is retired as a build and kept as the spike's record |
| 12.5 | Whether the scripting library's source files, which the SDK ships to modders, may be committed, or must be copied from the player's install at build time | Owner | Script packages; the translator | Open; the build copies from the install until decided |
| 12.6 | Which transport Dynaverse missions use: the DirectPlay wrapper or `nSwitch` | Analysis (`analysis/netcode/`) in Phase 3 | The Phase 4 transport design | **Answered 2026-11-07 (N1, `analysis/netcode/dynaverse.md`):** the DirectPlay wrapper through `tNetControl`. `tState_RunCampaign` hosts or joins over TCP/IP, by connect-by-address to the host address in the server's `tMatchedMission`. nSwitch carries the matching and the results; during the mission the client has it cache Dynaverse messages, and (inferred, from static analysis) carries no combat traffic. A Phase 4 server writes the host's rendezvous name into that field |
| 12.7 | Whether the lifted build's 53-bit floating point reproduces the original's traces | The oracle, once a trace exists | Any "functional" claim for simulation code in the native build | Open |
| 12.8 | NAT traversal: libjuice, GameNetworkingSockets, or the project's own punch over Transport | Integrator, in the Phase 3 protocol document | Phase 3 transport package | **Decided 2026-11-07:** the project's own punch over Transport, with its own relay (`src/net/RENDEZVOUS.md`, `NAT_LIBRARY.md`); no library. libjuice (pinned in `NAT_LIBRARY.md`) is the fallback if the punch fails N2's acceptance, taken by a contract change |
| 12.9 | Which machine records the oracle (a Windows PC, CrossOver, another Wine build) | Owner | Every "functional" figure | Open |
| 12.10 | Which community fixes default on and which are rules changes | Owner, reviewing `docs/FIXES.md` as it fills | Nothing until the fixes packages | Open |
| 12.11 | Whether the decompilation and the modernization split into two repositories | Owner, at the reveal | Nothing before it | **Decided 2026-11-16 (supersedes 2026-11-03):** split into SFC2-decomp (the decompilation, to be public) and SFCOP-port (this repository, private), which pins it as `decomp/` |
| 12.12 | Empires at War as a second module (§7.5): which build is the target (the 2000 retail exe or a later official patch), and when `Tiger/` and `dmp3d/` move to the shared tree | Owner, when that work starts | The second module; nothing before it | Open; Orion Pirates is the only target until then |
