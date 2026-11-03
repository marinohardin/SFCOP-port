# Research report: StarFleetOP.exe toolchain (Rich header) and macOS build/analysis options

Sub-agent report, 2026-11-02. URLs are the agent's citations; items it marked unverified are kept as such.

## 1. Rich-header build numbers -> MSVC 6 releases

Primary table: dishather/richprint `comp_id.txt` (https://raw.githubusercontent.com/dishather/richprint/master/comp_id.txt). Key: the Rich `build` is the C2/back-end (c2.dll/link) stamp, not the `cl.exe` banner, which is why SP5/SP6 cl.exe reports 12.00.8804 while objects carry 8966/9782.

- 8168 (0x0a/0x0b/0x04): VC6 RTM. Also "lib-rtm preserves build-8168 libraries" in https://github.com/sushi-shi/homm3-decomp/releases. The 6/12 objects at 8168 = RTM-built CRT/libs.
- 8447 (0x04 Linker600 / 0x05 Cvtomf600 / 0x0b): VS6 SP3 compiler; the linker stamp stayed 8447 from SP3 through SP5/SP6. homm3 release confirms SP3 = "C1/C1XX 12.00.8472; C2/LINK 12.00.8447".
- **8799 (0x0a/0x0b, bulk = 1062 C++ objects): VS6 SP4** (`000b225f [C++] VS6 SP4 build 8799`). The game itself was compiled with VC6 SP4 (cl.exe banner for SP4 not confirmed by a primary source).
- 8797: `VS6? (Likely Libs) build 8797`, an SP4-era pre-release back-end used to build SP4's CRT/MFC libs.
- 8966: VS6 SP5 (cl.exe banner 12.00.8804; Wikipedia `_MSC_FULL_VER 12008804` for SP5/SP6). 16/3 objects = a third-party lib built with SP5.
- 9044 (0x30/0x31 = Utc12_2_C/CPP, "VS98 6.20"): VC6 SP5 + Processor Pack. (SP4+PP final was 8943, per tomsons26's list quoted at https://krystalgamer.github.io/open-tobey-september-2025.) Only 10 objects.
- SP6 would be 9782, absent here, consistent with the Oct 2003 link date (SP6 shipped 2004).
- 0x13 = Linker512 (VS97 5.12 linker) builds 8034/9049: import libs built by a VC5 LIB.exe (e.g. DirectX/SDK import libs).
- 0x1c/0x1d = Utc13_C/CPP build 9178: VS2002 XP SP1 DDK build; one C and one C++ object from a VS2002-DDK-compiled static lib.
- 0x19 = Implib700 build 9210: VS2002 import library (DirectX 8.1 SDK plausible; unverified).
- 0x0e Masm613 7299 = MASM 6.13 CRT asm objects; 0x0c AliasObj60 7291 = OLDNAMES.LIB; 0x06 Cvtres500 1735 = CVTRES 5.00.1735.
- Microsoft KB Q194295 only describes a method: https://ftp.zx.net.nz/pub/archive/ftp.microsoft.com/MISC/KB/en-us/194/295.HTM

## 2. Ready-made toolchains with the exact compiler

- decomp.me (`values.yaml`, https://raw.githubusercontent.com/decompme/compilers/main/values.yaml): win32 ids `msvc6.0`, `msvc6.3`, `msvc6.4`, `msvc6.5`, `msvc6.5pp`, `msvc6.6` (plus 7.0, 7.1), from https://github.com/OmniBlade/decomp.me/releases/tag/msvcwin9x. Naming = SP level: **6.4 = SP4 = build 8799**. Corroborated by maci0's README.
- maci0/recompile-toolchains (https://github.com/maci0/recompile-toolchains): images `rebrew/msvc:6.0-win32` etc.; MSVC 6 variants msvc600 (+sp5, sp5_vcpp, sp6), msvc600_sp1..sp4. Runner `REBREW_RUNNER=wine|wibo`. Native binaries are x86_64 Linux.
- itsmattkc/MSVC600 (https://github.com/itsmattkc/MSVC600): portable VC6 command-line tools; SP level not stated (unverified) — check the Rich stamp of a test object.
- jaenster/setup-msvc (CI action; VC6 only as RTM so far) https://github.com/jaenster/setup-msvc ; ahaczewski/GeneralsToolchain = VC6 SP6 + DX8.1 SDK.

## 3. Running MSVC 6 on Apple Silicon

- wibo (https://github.com/decompals/wibo): runs 32-bit Windows command-line binaries on Linux and macOS. `wibo-macos` release is x86_64 only, "experimental, Rosetta 2 supported"; no native arm64-macOS build. Builds on macOS (PR #138, Sep 2026).
- MSVC status: 1.2.0 (Jul 2026) improved kernel32 for MSVC tools; `link.exe` needed `lstrcmpiW` (issue #105), fixed on `main` by PR #141 (merged 2026-09-25). So cl.exe works on 1.2.0, link.exe needs a post-1.2.0 build. PR #146 (VC6 precompiled headers) open; #115 "msvc600: cannot open file TEMPFILE" open.
- No first-hand report of MSVC 6 under wibo specifically on macOS (unverified; expected to behave like Linux x86_64 wibo). pokebw2 uses x86-64 wibo under Rosetta for Metrowerks.
- Wine on macOS: via Rosetta 2; WoW64 runs 32-bit EXEs. Gcenx builds https://github.com/Gcenx/macOS_Wine_builds ; Homebrew cask `wine-stable` disabled since 2026-09-01 (fails_gatekeeper_check). VC6-under-Wine precedent (Linux): https://github.com/ryzom/ryzomcore/wiki/vc6_wine_environment
- CrossOver 27 (2026): Apple-silicon-only, 32-bit apps in WoW64 bottles; native arm64 preview Aug 2026. Rosetta retirement reported for macOS 27/28; all paths depend on Rosetta.
- Docker: amd64 containers run on Apple Silicon via Docker Desktop "Use Rosetta for x86_64/amd64 emulation".

## 4. Ghidra vs IDA for MSVC 6 x86

- Ghidra 12.2 released 2026-10-22 (JDK 25). Built-in "Windows x86 PE RTTI Analyzer" plus `RecoverClassesFromRTTIScript`. Note VC6 RTTI is only emitted with `/GR` (Taldren used /GR per the .dsp files).
- OOAnalyzer (cmu-sei/pharos): 32-bit MSVC x86 only, still maintained (commits Oct 2026); import JSON via CERT Kaiju (release 261023 supports Ghidra 12.2). https://github.com/cmu-sei/pharos ; https://github.com/certcc/kaiju
- ghidra-delinker-extension: COFF x86 and x86_64, ELF, OMF. Latest v0.9.3 (2026-10-30, Ghidra 12.2). Relocation synthesizer depends on Ghidra DB accuracy; re-run analysis before export; plugin enabled under Experimental. https://github.com/boricj/ghidra-delinker-extension/releases
- IDA: not researched beyond scope.

## Bottom line
Game = VC6 SP4 (C2 build 8799, link 8447) with RTM CRT (8168), SP4 pre-release libs (8797), an SP5 lib (8966), an SP5+PP lib (9044), a VS2002-DDK lib (9178) and VS2002/VC5 import libs. Matching toolchain: decomp.me `msvc6.4`. On Apple Silicon: x86_64 wibo under Rosetta for cl.exe (build from `main` for link.exe), Wine/Gcenx or Docker+Rosetta as fallbacks; everything is Rosetta-dependent.
