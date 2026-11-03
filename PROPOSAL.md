# Orion Pirates: how to decompile and port it with bounded time and tokens

Date: 2026-11-02. Author: Claude (Fable 5.1) with Marino. Supersedes the approach in
`reference/SFC2 Orion Pirates decomp workflow.md` where the two disagree; that document's
survey of decomp projects and harness design remains valid and is referenced rather than repeated.

## 1. Summary

Orion Pirates is a far better decompilation target than the earlier plan assumed, because the
game ships most of its own symbols. The official modding SDK in the install folder
(`OP API - R2.1`) contains five MSVC 6 release-mode static libraries built from the same source
tree, with roughly 60,000 mangled C++ names, plus 12,600 lines of real Taldren headers. Matching
that library code byte-for-byte into the patched 2.5.5.2 exe already names 3,758 functions with
zero reverse engineering, and a further ~3,000 identical template instantiations are identifiable
as a group. The exe also carries 1,983 RTTI class names and 229 source-file paths from assertion
strings, so the translation-unit layout is recoverable.

Two of the four goals are also cheaper than thought. The game uses raw Winsock (no DirectPlay),
so there is no dead middleware to replace, only Taldren's own "Switch" network layer to modify.
Dynaverse II login goes through Sierra's WON directory servers whose addresses live in a plain
text file (`WONServerSetup.gf`), so re-pointing needs no patching, and both the WON protocol
headers (Homeworld source release) and an open-source WON server (2026) exist. The official
server kit runs with a flat-file database and needs no SQL Server.

Priorities (set by Marino, 2026-11-02): **a native macOS client first**, NAT-friendly netplay
second, a self-hosted Dynaverse II last. The work stays private until it is ready to be shown.

Recommendation: a **symbol-first functional decompilation** whose first milestone is a client that
runs natively on Apple Silicon: (0) recover symbols, pin the toolchain, build the replay oracle;
(1) a time-boxed spike that picks the fastest route to a native build, static recompilation of
the whole exe versus delink-and-replace, and establishes an "always runs on macOS" baseline;
(2) rebuild the platform-facing subsystems (renderer, sound, input, window, file, registry) as
readable source on SDL3, and grind the rest with a harness-enforced Claude Code loop, matching
bytes where the SP4 compiler cooperates; (3) netplay and server chooser in source; (4) Dynaverse
II (WON directory and auth emulation plus the official server kit). Byte-matching is a
verification oracle, not the goal.

## 2. What the binaries actually are (verified locally today)

All numbers below come from scripts in `analysis/scripts/`; outputs are in `analysis/results/`.

### 2.1 The files

| File | Role | Size | Linked | Notes |
|---|---|---|---|---|
| `StarFleetOP.exe` (installed, 2.5.5.2) | **The game** | 4.8 MB, .text 3.75 MB | 2003-10-17 | Target of all work. No `.reloc` section (fixed base 0x400000). |
| `SFCOP.exe` | Boot stub | 184 KB | 2001-05-23 | Reads registry, plays `sfc2intro.exe`, launches StarfleetOP.exe. Debug build (`boot2.pdb` path). Not the game. |
| `StarFleetOP.exe` (disc, 2.5.0.0) | Original retail game | 8.1 MB, .text 5.3 MB | 2001-06-20 | Same translation units but less optimised; matches SDK libs worse than 2.5.5.2. Also bundled in the server kit for client-file validation. |
| `ServerPlatform.exe` | Dynaverse II server kit (v2552 build 37) | 3.1 MB .text | 2001-06-28 | Has `.reloc`. Shares the `Common`, `Meta`, `Switch` code with the client; 3,100 SDK-lib functions match it. |
| `sfc2intro.exe` | Intro movie player | 11.5 MB | 2001 | UPX-packed; irrelevant. |

### 2.2 Compiler and link environment

The Rich header of 2.5.5.2 says Visual C++ 6.0 (linker 6.0). The bulk of the game is 1,062 C++
objects from compiler build **12.00.8799** plus 33 C objects from the same build; the CRT and
libraries come from builds 8797, 8168, 8447 and 8966, with a handful of VC7-era import libs
(builds 9178/9210) and a Processor-Pack-era compiler (9044) on ten objects. The SDK's `.dsp`
project files give the exact flags Taldren used for the shared code:
`/MT /W3 /GR /GX /O2 /D WIN32 /D NDEBUG /D _MBCS` (static CRT, RTTI on, exceptions on, /O2).
Mission scripts are plain DLLs (`.scr`, 90 of them in `Assets/Scripts`) linked against the same
libraries, so a script is also a ready-made test of the compiler setup.

### 2.3 Imports and middleware

| DLL | Used for | Porting implication |
|---|---|---|
| `WSOCK32` (28 fns) | All networking: TCP and UDP sockets, `select`, `gethostbyname` | **No DirectPlay.** Transport is Taldren's own `nSwitch` layer over BSD sockets; portable almost as-is. |
| `DDRAW` (3 fns) + D3D7 via QueryInterface | Rendering through Quicksilver's `dmp3d` engine (`d3dViewMgr`, `tDDSurfMgr`, `drawPoly`) | The one subsystem that must be rewritten for the port (SDL3 GPU or a DX7-to-modern shim). |
| `mss32.dll` (Miles Sound System, 26 fns) + `DSOUND` | Audio through QSI's "Tiger" sound layer (`tSndMgr`, `tAudioChannel`) | Tiger is 1,852 functions matched from the SDK lib; swap Miles for miniaudio behind it. |
| `DINPUT` | Input | Trivial to replace. |
| `SHW32.DLL`, `HA312W32.DLL` | MicroQuill SmartHeap 6.0 allocator | Drop; use the system allocator. |
| `DBGHELP`, `BugslayerUtil.dll` | Crash call-stack reporting (`ErrorReport.exe`) | Drop or keep. |
| `KERNEL32/USER32/GDI32/ADVAPI32/ole32/WINMM/comdlg32` | Window, registry, timers, file dialogs | Standard SDL/portability layer work. |

### 2.4 Source tree recovered from strings

Assertion strings embed the original paths. The game is `F:\Projects\Taldren\Projects\...`:

- `SFC2_5\Meta\...` (116 paths): the Dynaverse II client: `MetaDatabase\AsyncDatabaseProcedures\*`, `Servers\WonLogon.cpp`, `WON\WONInterSwitchConnection.cpp`, auctions, prepared missions, political tension matrix.
- `SFC2_5\Source\...` (56 paths): the game proper.
- `Common\{Database,DataStore,Thread,Timer,Stats,Debug,Settings,OSVersion}`: Taldren's utility layer; **this is `SharedR.lib`** and is largely matched.
- `Switch\*.cpp` (12 files): `Switch, SwitchAddress, SwitchDirectory, InterSwitchConnection, InterSwitchServer, CentralObjectDirectory, RequestWithResponse, AsyncProcedure, DataHandler...`: **the network layer**.
- `D:\work\QSI\Projects\CORE\dmp3d\...` and `QSIInc\Tiger\...` (15 paths): Quicksilver Software's engine (3D) and platform (sound, memory, paging) libraries inherited from Starfleet Command 1.

Namespaces visible in RTTI: `nSwitch`, `nDatabase`, `nDataStore`, `nLog`, `WONAPI`, `CryptoPP`, `std`.

### 2.5 The symbol windfall: SDK static libraries vs the exe

`analysis/scripts/libmatch.py` parses each COFF archive, takes every per-function COMDAT code
section, masks relocation slots, and searches the exe's `.text` for the exact bytes.

| Library | Code sections | Unique byte-exact match in 2.5.5.2 | Identical-group ("multi") | Meaning |
|---|---|---|---|---|
| `sfcTiger_MSVCR.lib` (QSI Tiger) | 3,391 | **1,852** | 532 | Platform/sound layer essentially free. |
| `ScriptInterfaceR.lib` | 4,908 | 904 | 878 | Mission-script interface, ship/team/mission structs. |
| `SharedR.lib` (Taldren Common + Scripting + Network structs) | 4,005 | 843 | 723 | Database, DataStore, Thread, Log, `NetMsgClass`, `MapObject`, `MissionSpec`. |
| `cryptlibR.lib` (Crypto++) | 4,482 | 139 | 834 | Open-source library; recompile from source instead of matching. |
| `dmp3D_MSVCR.lib` | 86 (archive truncated) | 20 | 7 | File is damaged; only 9 objects parse. |
| **Total** | 16,872 | **3,758** (12.6% of .text bytes) | 2,974 | Plus "nomatch" functions that changed between 2001 and 2003 or were inlined; a relocation-aware fuzzy pass will recover many. |

"Multi" means the same bytes occur at several exe addresses (STL template instantiations, small
accessors); the name is known, only the address is ambiguous, which a call-graph pass resolves.
The 2.5.0.0 disc exe matches fewer (2,989) because it was built with different optimisation; the
2003 patch build is the better target on every axis.

What this buys: the Ghidra project starts with ~4,000 correctly named and prototyped functions,
~2,000 classes with RTTI, real headers for the mission/ship/team/map structures, and a map from
functions to original `.cpp` files. CMR2Decomp, the closest published analogue, started with none
of these and still reached a playable build.

### 2.6 Networking and Dynaverse facts

- Skirmish/LAN/Direct-IP play and Dynaverse missions both use `nSwitch` over Winsock. NAT
  problems are therefore about Taldren's connection set-up, not DirectPlay's; the fix is a
  transport change inside one subsystem (relay or ICE), plus a lobby.
- Dynaverse II login: `tWonLogon` uses `WONAPI` (AuthServer, DirServer, firewall detector;
  error table `WS_AuthServ_*`, `WS_PeerAuthServer_*`). Directory hosts are **config, not code**:
  `MetaAssets/ServerProfiles/Multiplayer/WONServerSetup.gf` lists
  `sfc2.{west,central,east}.won.net:15101` and the directory path `/StarFleetCommand2/Game/Release`.
- GameSpy Arcade: hostnames `master/gpcm/gpsp/key/motd.gamespy.com` are string literals in the
  exe; the GameSpy SDK (QR, GP presence, peerchat) is compiled in. OpenSpy implements all of it.
- Server kit: `ServerPlatform.exe` + `ProcessSentinel.exe` watchdog; `Assets/ServerProfiles/Database.gf`
  has `UseSQL = 0` (flat file) with an optional ODBC DSN. It only needs the WON directory and
  the game clients to find it. The "MESSAGE DLL" and documentation `.doc` are in the kit.
- Community state (from research): dynaverse.net resolves to nothing as of today; D4v1ks's
  closed-source **SFC Launcher** (2016-2017) emulated the WON directory on 15101/26100 and
  GameSpy on 28900/29900/29901/6667, but downloads are gone and the 2.564 server kit was never
  public. No public Dynaverse II server was found. No GitHub project reverse-engineers SFC2/OP;
  `Revan67/SFC3Dynaverse` (2026) does SFC3's GameSpy side only.

### 2.7 Size estimate

Heuristics on 2.5.5.2: 14,174 distinct `call` targets, 2,961 `push ebp; mov ebp,esp` prologues
(most functions are frame-pointer-omitted), 4,217 `int3` padding runs. A realistic total is
15,000 to 25,000 functions, of which roughly a third are CRT, Dinkumware STL, Crypto++, WONAPI
and GameSpy SDK code that has public source or is already named. The earlier plan's 3,000 to
6,000 estimate was low by a factor of three to five; the compensating fact is that the share
needing genuine reverse engineering is much smaller than in a typical stripped exe.

## 3. What comparable projects teach (research, Nov 2026)

Four research passes were run today (toolchain, symbol recovery, WON/GameSpy/Dynaverse, and
the state of x86 decomps). The findings that change the plan:

**Compiler.** Rich-header build 8799 is the C2 back-end stamp of **Visual C++ 6.0 SP4**
(dishather/richprint `comp_id.txt`); the linker stamp 8447 is the SP3-to-SP5 linker. decomp.me's
win32 platform ships this exact compiler as `msvc6.4` (OmniBlade tarballs), and
maci0/recompile-toolchains has Docker images for every service pack. SP6 (build 9782) is absent,
as expected for an October 2003 link. The SDK's ScriptInterface sources (32 `.cpp` files) plus
the compiled `ScriptInterfaceR.lib` give a built-in calibration test: compile them with the
pinned compiler and the `.dsp` flags and diff against the library in objdiff before touching the
game. No other project found had ground truth like this.

**Running MSVC 6 on a Mac.** wibo (the decomp community's Win32 PE loader) is x86_64-only on
macOS and runs under Rosetta 2; `cl.exe` works, `link.exe` needs a wibo built from `main` after
September 2026 (the `lstrcmpiW` fix). Fallbacks are Gcenx Wine builds (the Homebrew cask is
disabled) or amd64 Docker with Rosetta. Every option depends on Rosetta, which Apple is retiring
after macOS 27, so the build oracle should live in a Linux x86_64 container or CI runner from the
start, with the Mac as the editing and analysis machine.

**Symbol recovery tooling.** Ghidra 12.2 (Oct 2026) ships the MSVC RTTI analyzer,
`RecoverClassesFromRTTIScript`, and a Function ID (FID) system with scripts (`ImportMSLibs.java`,
`CreateMultipleLibraries.java`) that ingest `.lib` archives and build relocation-tolerant
signature databases; the bundled database already covers the VC6 CRT. OOAnalyzer (CMU, still
maintained) targets exactly 32-bit MSVC C++ and imports into Ghidra through Kaiju. objdiff 3.8.3
diffs x86 COFF natively and can take a library member as the target with no build step, which is
how we will verify rebuilt library code. reccmp compares whole binaries only. Public precedents for
recovering a game's own symbols from a shipped mod-SDK library were not found; Halo 2, PS1 PsyQ
and Xbox XDK projects did it for SDK code and call it "a critical starting point".

**Precedents that match this target.** openblack/bw1-decomp (Black & White, 2001, MSVC 6
SP4/SP5 C++) is the closest matching-style template: decomp-toolkit fork splits the PE into
objects, objdiff scores, `build.sha1` certifies. CMR2Decomp plus pablocpas/OpenCMR2 is the
closest port template and the same API stack (DirectDraw, D3D7, DirectSound, DirectInput, UDP
networking): OpenCMR2 v1.0 ships natively on Linux, Android and Windows from a **partial**
decomp by keeping `game/` source-identical, tagging every port edit `PORT:`, and using
bit-exact replay of 500 vehicle updates as the oracle. ecruells' Resident Evil PC decomp went
from Ghidra to a playable cross-platform functional build in about five months. isle-portable
is the model for the platform layer (SDL3, SDL GPU, miniaudio, iniparser, D3D reimplementation).
FUEL (fmtk) proves delink-and-relink on a real game and documents its pitfalls; MSVC 6 code
without LTCG is far friendlier than FUEL's. No project has documented byte-matching heavy
Dinkumware STL template code, so STL-heavy units are a matching risk and should be rebuilt from
the SDK's own STL headers instead.

**AI-driven decomps and their cost.** momo5502's unnamed FPS: 16 agents, three months, **600
to 700 billion tokens**, 99% of functions present and 83% byte-exact; the lessons were a
machine-checkable byte oracle, a hashed verifier the agents cannot edit, and treating worker
comments as untrusted. decompdecomp (successor to decomp.wtf) automates "easy functions first,
original compiler judges" but no whole game has finished through it. decomp.dev closed its public
index in October 2026 over hostile AI forks; decomp.me and the tools remain. These numbers rule
out a full byte-matching decomp on a personal budget and argue for functional equivalence with
matching used only where the SP4 compiler reproduces cleanly.

**Static recompilation moved in 2026.** sp00nznet/pcrecomp (Capstone lifters plus a Win32
high-level emulation layer on SDL2) produced Red Alert 2 Recompiled (24.9k functions, native
Windows/Linux/macOS, campaigns playable, LAN via IPX emulation) and NFSU2 Recompiled (27.7k
functions, 8.9M lines of C, with an arm64 macOS Metal fork). Its HLE layer stops at DirectDraw 2
and DirectSound 1, so Orion Pirates' D3D7 renderer is out of scope today. Recomp is a credible
hedge for the native port, not a route to maintainable netcode.

**Dynaverse and WON.** Sierra's WON was never open-sourced, but Homeworld's 2003 source release
carries the complete Titan protocol headers (`src/Titan/msg/{Auth,Dir,Routing,Firewall}`,
ElGamal/Blowfish crypto wrappers), the community `madebr/WON` archive holds recovered WONAPI SDKs
(Dark Reign 2 source, AVP2), and **FlashZ/won_oss_server (Python, AGPL, pushed Sept 2026)** is a
working clean-room WON Auth1/Directory/Routing/Factory/Firewall server for Homeworld. OpenSpy
implements every GameSpy service the exe uses (QR 27900, browsing 28900, GP 29900/29901,
peerchat 6667) and is alive. D4v1ks's SFC Launcher proved in 2016 that emulating the directory on
15101 plus GameSpy is enough to run OP campaigns; its code is not public. The official server kit
defaults to a flat-file database (`UseSQL = 0`). Nothing public runs a Dynaverse II today.

## 4. Options compared

| Approach | Self-hosted Dynaverse | NAT-friendly netplay | Native macOS/Linux | Fixes and features | Cost and risk |
|---|---|---|---|---|---|
| A. Wine/CrossOver + emulated WON/GameSpy servers, no code changes | **Yes, weeks** | No (same sockets) | Rosetta only | None | Very low; depends on Rosetta's future |
| B. Proxy DLL (`wsock32` shim) for relay/hole-punch + launcher | Yes | Partial (transport only, no lobby) | No | Tiny | Low-medium; dead end for the port |
| C. **Symbol-first functional decomp, delink-and-relink, port last** | Yes (A first, then in source) | Yes, in source | Yes, at the end | Best | Medium-high, incremental, always playable |
| D. Full byte-matching decomp, then port | Late | Late | Late | Yes | Hundreds of billions of tokens at momo5502's rate; STL matching unprecedented |
| E. Static recompilation (pcrecomp-style) | Via A | Hard (lifted C is unreadable) | Plausible but D3D7 HLE must be written | Poor | High toolchain work; good hedge only |
| F. Clean-room reimplementation | New design | Yes | Yes | Fidelity risk | Years |

Recommendation: **C as the destination, with E (static recompilation) evaluated head-to-head
in a short spike as the fastest way to the first native macOS build, and A deferred to the end
as the Dynaverse phase.** A native Apple Silicon client requires every byte of x86 in the exe to
be replaced by something a Mac compiler can build. There are only two ways to get there: write
all of it as source (C, 12 to 18 months), or lift all of it mechanically to C and then replace the
lifted code with readable source where you need to change behaviour (E then C). Red Alert 2
Recompiled and NFSU2 Recompiled reached playable native builds of 25,000-function MSVC 6 games
in about eight months in 2026 by the second route, and a macOS arm64 Metal fork of NFSU2 exists.
The route cannot be chosen from research alone because the known blockers (MSVC C++ exception
tables, which this exe uses heavily via `/GX`, STL and Crypto++, and a Direct3D 7 high-level
emulation that pcrecomp lacks) can only be sized against this binary.

The other corrections to the earlier plan stand: symbol recovery from the SDK libraries comes
before anything else, library code (CRT, STL, Crypto++, WONAPI, GameSpy SDK, Miles, SmartHeap)
is rebuilt from public source or replaced rather than decompiled, and byte-matching is a free
oracle because the token cost of matching everything is now documented.

## 5. Recommended phases

### Phase 0: foundations (3 to 4 weeks, low token spend)

Deliverable: a private repo whose CI compiles with VC6 SP4, a Ghidra project with roughly 4,000
named functions and recovered classes, and a replay oracle recorded against the untouched game.

1. **Repo hygiene.** Private repository; no retail assets or binaries, only SHA-256 hashes of
   `StarFleetOP.exe` 2.5.5.2 and the SDK libs; `CLAUDE.md` with the ownership rules; hooks that
   deny edits to hashes, generated asm, the verifier and compiler flags.
2. **Toolchain.** decomp.me `msvc6.4` tarball in a Linux x86_64 Docker image (wibo from `main`
   for `link.exe`, Wine fallback), driven from the Mac. Calibration: build the 32 ScriptInterface
   `.cpp` files with the `.dsp` flags and objdiff them against `ScriptInterfaceR.lib`. Target: a
   high byte-exact rate; whatever does not match tells us the exact flag or SP mismatch before any
   game code is written.
3. **Symbols.** Ghidra 12.2 import with RTTI analyzer and class recovery; OOAnalyzer via Kaiju;
   FID database from the five SDK libs; apply the exact-match table in `analysis/results/`;
   propagate names through callers and vtables; map functions to translation units via the 229
   assertion paths; export `functions.csv` (address, size, name, TU, library/game flag). Pin the
   Crypto++ version against `History.txt` and verify the WONAPI SDK source provenance.
4. **Replay oracle.** A custom mission script (`.scr`, built with the same SDK) logs per-tick
   ship state through the scripting API against the untouched exe; a scripted skirmish with fixed
   inputs gives a checksum trace that never changes. This is the behavioural judge for everything
   that does not byte-match, and the only judge once the code runs on arm64.
5. **Baseline on the Mac today.** Install the untouched game under CrossOver or Gcenx Wine and
   confirm the replay oracle reproduces there. This is the comparison point for every later build
   and the fallback players have at any moment.

### Phase 1: the route spike (3 to 5 weeks, decides the shape of Phase 2)

Deliverable: a written decision, backed by measurements on this exe, between two routes to the
first native macOS build, plus whichever baseline the decision picks.

- **Route L (lift).** Run a pcrecomp-style x86 lifter over `StarFleetOP.exe` and compile the
  output for arm64. Measure: how many of the ~15,000 to 25,000 functions lift cleanly; what the
  lifter does with MSVC C++ exception frames, `__thiscall` virtual dispatch and the RTTI tables;
  how much of the Win32 surface is already covered by its HLE layer (Winsock, GDI, registry,
  DirectSound, DirectInput are; Direct3D 7 is not). Prototype the missing DDraw7/D3D7 HLE against
  dmp3d's three or four entry points (`tD3DViewMgr`, `tDDSurfMgr`, `drawPoly`) using OpenCMR2 and
  isle-portable as reference. Goal: the main menu renders natively on the Mac.
- **Route D (delink).** Delink the exe with ghidra-delinker-extension 0.9.3 (FUEL recipe: clean
  xrefs, every byte in a function, CRT initialisers, resources via `rc.exe`), relink at another
  base, boot to menu and pass the oracle. This yields a rebuildable x86 Windows exe that runs on
  the Mac through CrossOver and lets source replace objects one at a time, but it is not native
  until the last object is gone.
- Decision rule: if Route L boots to the menu within the spike and exception handling is
  tractable, Phase 2 starts from the lifted tree and replaces lifted functions with readable
  source in priority order (always native, always playable). Otherwise Phase 2 starts from the
  delinked tree and the native build arrives only at its end; both routes share the same source
  tree, oracle and harness, so nothing from the spike is wasted.

### Phase 2: rebuild, platform first (6 to 12 months, the token-heavy phase)

Deliverable: a client that runs natively on Apple Silicon, Windows and Linux and passes the
replay oracle, with the platform layer and the subsystems you intend to change written as
readable source.

1. **Platform layer in `src/` first**, because it is the part that cannot be lifted and is the
   part that makes the build native: SDL3 window, input and timers; SDL GPU (Metal on macOS,
   Vulkan and D3D12 elsewhere) or an OpenGL backend behind dmp3d's `tD3DViewMgr`/`tDDSurfMgr`
   interfaces; miniaudio behind Tiger's `twSoundPart` (Tiger itself is 1,852 functions already
   named from the SDK lib); iniparser for registry and `.gf` files; the system allocator in place
   of SmartHeap; Winsock mapped to BSD sockets.
2. **Game code in `game/`**, replaced in this order: anything the platform layer touches
   (window, file, timer, memory glue), then the simulation and UI functions that coverage traces
   of a skirmish hit, then the long tail. Queue ordering by coverage (DynamoRIO or a Wine-side
   tracer), then by similarity to solved functions.
3. One fresh headless Claude Code session per function or small cluster; the only judge is the
   build-and-verify script (compile for x86 and arm64, objdiff or reccmp where the SP4 compiler
   applies, replay checksum, never regress); hooks and a reviewer sub-agent as specified in
   `reference/SFC2 Orion Pirates decomp workflow.md`.
4. Model tiering: Sonnet or Haiku for functions that already have a name, a prototype and a
   matched sibling; Opus for the hard tail; nothing above Opus in the loop.
5. Library code is never decompiled: CRT and STL from the MSVC 6 SP4 and SDK headers, Crypto++
   from the pinned release, WONAPI and GameSpy SDK from recovered source, Miles replaced by
   miniaudio. Roughly a third to 45% of the function count leaves the queue.
6. `game/` stays source-identical to the original; port-motivated edits are tagged `PORT:`
   (OpenCMR2 convention). 64-bit and LP64 hygiene from the start: explicit serialisation for
   every on-disk and on-wire struct (`DataStore`, `NetMsgClass`), no `long` in wire formats.
7. CI matrix (macOS arm64, Windows x64/arm64, Linux x64); Developer ID signing and notarisation
   on macOS when the first build goes to anyone else.

### Phase 3: netplay and server chooser (2 to 3 months, overlaps the tail of Phase 2)

- Decompile the 12 `Switch\*.cpp` units (nSwitch: addresses, connections, directories, async
  procedures) and the GameSpy glue as readable source if Phase 2 has not reached them.
- Server list read from config with a chooser UI; a transport option in nSwitch that goes through
  a community relay or ICE (libjuice or GameNetworkingSockets, both researched in `reference/`);
  the known crash fixes from the community 2.564 package.

### Phase 4: Dynaverse II (1 to 3 months)

- Run the official `ServerPlatform.exe` kit (flat-file database, `ProcessSentinel.exe` watchdog)
  under Wine on a small Linux host; adapt `FlashZ/won_oss_server` to the SFC2 directory path and
  port 15101 for WON directory, auth and firewall detection; OpenSpy for GameSpy lobby and chat;
  the client's `WONServerSetup.gf` points at them. This works with the stock 2.5.5.2 client too.
- Then, with `Meta\*` decompiled, replace the WON client calls with a plain protocol of your own
  and write the campaign server in your stack (.NET or TypeScript) using the decompiled
  `MetaDatabase` procedures as the specification. `ServerPlatform.exe` shares 3,100 matched SDK
  functions and the whole `Meta` tree with the client, so it is largely readable by then.

## 6. Time and token budget

| Phase | Calendar | Human hours | Token profile |
|---|---|---|---|
| 0 Foundations | 3 to 4 weeks | 40 to 60 | Low: scripting and configuration. Tens of millions of tokens. |
| 1 Route spike | 3 to 5 weeks | 30 to 60 | Low to medium: tooling work, one D3D7 HLE prototype; a few hundred million tokens at most. |
| 2 Rebuild, platform first | 6 to 12 months | 2 to 4 hours/week review | High: 10,000 to 14,000 functions after library removal. At 100k to 300k tokens per functional match this is 1 to 4 billion tokens, i.e. most of a Max 20x seat for the duration, with Opus reserved for the tail. On Route L the native build exists from month two or three and this phase makes it readable; on Route D it arrives at the end. Insisting on byte-exact matching would multiply the cost by 5 to 10 (momo5502 data point). |
| 3 Netplay + chooser | 2 to 3 months | 40 to 80 | Medium: ~1,500 functions in nSwitch and GameSpy glue plus new transport code. |
| 4 Dynaverse | 1 to 3 months | 40 to 80 | Low to medium: mostly configuration and backend work in your own stack. |

Milestones with the macOS-first ordering: a native Apple Silicon build that boots to the menu in
two to three months if Route L works, otherwise at the end of Phase 2 (12 to 18 months); a fully
playable native client that passes the replay oracle at the end of Phase 2 either way; NAT-friendly
netplay and a server chooser two to three months after that; a self-hosted Dynaverse last. The
cost of this ordering is that nothing is shareable with players for most of a year, which suits a
private build-up toward a single reveal. The budget levers, in order of impact: skip library code
(done by design), accept functional equivalence over matching, use the FID names and real headers
so the model sees typed code instead of guessing, tier models, and cap attempts per function with a
`difficult_functions` list for human triage.

Infrastructure cost is near zero: the VC6 SP4 toolchain runs in a free amd64 container on the Mac
under Rosetta today (Docker Desktop, OrbStack or Colima) and unchanged on GitHub Actions' free
x86_64 runners; when Rosetta is retired, a 4-euro-a-month x86 VPS or Oracle's free tier hosts the
compile oracle. Ghidra and the arm64 builds run natively on the Mac.

## 7. Risks and open questions

- **Rosetta dependence of every Mac path to MSVC 6.** Mitigation: Linux x86_64 build container
  or CI runner from day one.
- **STL-heavy translation units.** No public project has byte-matched Dinkumware templates with
  VC6; treat matching as unavailable there and rely on the replay oracle. The SDK's "updated
  STL" headers in `OP API - R2.1/STL/Include` are the exact headers Taldren compiled against.
- **The 2001 versus 2003 gap.** The SDK libs date from June 2001; 1,491 SharedR and 1,890
  ScriptInterface functions do not match the 2003 exe. A relocation-aware fuzzy pass (Ghidra
  Version Tracking "Exact Function Instructions Match", or FID with lower thresholds) should
  recover most; the remainder changed in patches and must be read.
- **`dmp3D_MSVCR.lib` is truncated** in both the installed copy and (apparently) the disc. The
  renderer therefore has the fewest free symbols and is the hardest subsystem; it is also the one
  the port replaces, so a functional reimplementation against dmp3d's interfaces is acceptable.
- **Legal.** Paramount owns Star Trek, ADB the Star Fleet Universe layer, Interplay the game; SFC2
  and OP are sold nowhere. Keep the repo private until the posture is decided, ship code only,
  require retail files by hash, and verify the licence and provenance of the WONAPI and GameSpy
  SDK sources before linking them. This is not legal advice.
- **Community.** The SFC community (Hot & Spicy Forums; D4v1ks, TarMinyatur) has hex-patched
  these binaries for a decade and holds the 2.564 server kit. The plan is a private build-up and
  a single reveal; when that happens, be explicit about AI-written source and avoid anything
  resembling the "hostile fork" pattern that closed decomp.dev. Their 2.564 fixes can be folded
  in from the public package without contact.
- **Route L specific.** MSVC C++ exception handling (`/GX`, used by STL, Crypto++ and Taldren's
  own `BaseException` hierarchy) is table-driven and the only public x86 lifter states it does not
  support it; a Direct3D 7 HLE does not exist and must be written; lifted C is unreadable, so
  every behaviour change still needs the corresponding functions decompiled. The spike exists to
  size these three before committing.
- **Open questions for week one:** exact `cl.exe` banner for SP4 and whether `/Og` style
  differences appear between the SDK libs and the exe; whether wibo-from-main links on macOS;
  how `ServerPlatform.exe` validates client files (`ValidatedClientFiles`, CRC) and whether the
  2552 build 37 kit accepts 2.5.5.2 clients; the WON Auth1 certificate flow for a game that is
  not Homeworld (the `WONServerSetup.gf` path differs, the message families should not).

## 8. Changes from the earlier plan

- DirectPlay is not involved; networking is Winsock through Taldren's `nSwitch`. The transport
  swap is smaller and the hostnames are in config files, not code.
- The function count is 15,000 to 25,000, not 3,000 to 6,000, but the SDK libraries, RTTI,
  assertion paths and public-source libraries remove far more of it than any precedent enjoyed.
- Symbol recovery and the compiler calibration test come before delinking.
- Order of goals is native macOS client, then netplay, then Dynaverse (Marino's priority), so
  the platform layer is rebuilt first and the server work moves to the end.
- Static recompilation is evaluated as the fast path to a native build in a time-boxed spike
  rather than kept only as a late hedge.
- WON and GameSpy servers are adopted (won_oss_server, OpenSpy) rather than written from scratch.
- Byte-matching is a verification aid where cheap; the project target is functional equivalence
  judged by compile plus replay, because the token cost of matching everything is now documented.
- Static recompilation is a credible 2026 hedge for the native build, with the D3D7 gap noted.

## 9. Key sources

- Rich header build table: https://raw.githubusercontent.com/dishather/richprint/master/comp_id.txt
- decomp.me win32 compilers: https://raw.githubusercontent.com/decompme/compilers/main/values.yaml ; maci0/recompile-toolchains: https://github.com/maci0/recompile-toolchains
- wibo releases and MSVC link fix: https://github.com/decompals/wibo/releases
- Ghidra 12.2, FID scripts, RTTI analyzer: https://github.com/NationalSecurityAgency/ghidra ; https://github.com/NationalSecurityAgency/ghidra/tree/master/Ghidra/Features/FunctionID/ghidra_scripts
- OOAnalyzer / Kaiju: https://github.com/cmu-sei/pharos ; https://github.com/certcc/kaiju
- ghidra-delinker-extension v0.9.3: https://github.com/boricj/ghidra-delinker-extension/releases
- objdiff: https://github.com/encounter/objdiff ; reccmp: https://github.com/isledecomp/reccmp
- bw1-decomp: https://github.com/openblack/bw1-decomp ; CMR2Decomp: https://github.com/CMR2Decomp/CMR2Decomp ; OpenCMR2: https://github.com/pablocpas/OpenCMR2
- isle-portable: https://github.com/isledecomp/isle-portable ; FUEL delink wiki: https://github.com/widberg/fmtk/wiki/Decompilation
- RE1 PC functional decomp: https://github.com/ecruells/resident-evil-pc-decomp
- momo5502 agent decomp write-up: https://momo5502.com/posts/2026-10-09-game-decompilation/
- pcrecomp / RA2 Recompiled: https://github.com/sp00nznet/pcrecomp ; https://github.com/sp00nznet/redalert2-recomp ; M-HT/SR: https://github.com/M-HT/SR
- WON: Homeworld Titan headers https://github.com/aheadley/homeworld ; recovered WONAPI SDKs https://github.com/madebr/WON ; open-source WON server https://github.com/FlashZ/won_oss_server
- GameSpy: OpenSpy https://github.com/openspy/openspy-core ; SFC3 Dynaverse reimplementation https://github.com/Revan67/SFC3Dynaverse
- SFC community: SFC Launcher thread https://hotandspicyforums.com/sfc-launcher-a-dynaverse-and-gamespy-s-service-rep-t19331.html ; OP 2.564 package https://hotandspicyforums.com/starfleet-command-orion-pirates-2500-2564-patch-t25292.html ; Dynaverse II kit archive https://archive.kontek.net/sfc.strategyplanet.gamespy.com/dyna_server.shtml
- Crypto++ history: https://github.com/weidai11/cryptopp/blob/master/History.txt
- decomp.dev sunset: https://decomp.dev/
