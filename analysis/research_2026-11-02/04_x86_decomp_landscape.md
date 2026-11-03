# Research report: Windows x86 (1996-2005) game decomp/recomp landscape, as of 2026-11-02

Sub-agent report. GitHub metadata from the REST API; items marked [unverified] come from one secondary source.

## isledecomp/isle (LEGO Island, 1997) + isle-portable
- Matching; CONFIG.EXE, ISLE.EXE, LEGO1.DLL completely decompiled; ReproBit reproduces all three byte for byte. MSVC 4.2, reccmp. Repo created 2023-04-28, 41 contributors. https://github.com/isledecomp/isle
- isle-portable (65 contributors, pushed 2026-10-30): SDL3, SDL GPU (Vulkan/Metal/D3D12), miniaudio, libsmacker, libiniparser, custom D3D Retained Mode reimplementation; CI for Windows x86/x64/arm64, macOS, Linux, many consoles. https://github.com/isledecomp/isle-portable
- Lesson: freeze the matching repo as the oracle; do portability downstream, one Windows subsystem at a time.

## CMR2Decomp (Colin McRae Rally 2.0, 2000) + pablocpas/OpenCMR2
- Decomp: matching with MSVC 6.0, reccmp; created 2025-08-24, 75 commits, "wip". https://github.com/CMR2Decomp/CMR2Decomp
- OpenCMR2 (created 2026-10-10, 210 commits, 2 contributors, v1.0): Clang/CMake/SDL3 port; replaced DirectDraw, Direct3D 7, DirectSound, DirectInput, DirectPlay (now UDP), Bink. `game/` stays close to original with every port edit marked `PORT:`; `src/` implements platform APIs. Linux x86-64 playable; Android playable; macOS/Linux ARM64 build and pass tests. Oracle: 16,896 startup math-table entries and 500 vehicle updates match bit-for-bit at multiple frame rates. https://github.com/pablocpas/OpenCMR2
- Lesson: a port can ship from a partial decomp if game code stays source-identical and port edits are tagged; use deterministic replay as the port's oracle. Closest analog to SFC:OP's API stack.

## AARosson48/SummonerDecomp (Summoner, 2001)
- Matching, MSVC 6 cl 12.00.8804 /O2 (itsmattkc/MSVC600), decomp-toolkit 0.0.27 split, objdiff 3.8.2; function counts only at 100%. Most C drafted with Cursor's agent, checked with objdiff. https://github.com/AARosson48/SummonerDecomp/blob/main/docs/matching.md
- Lesson: decompiler output lies about floats, inverted branches, return values; keep `fn_<addr>` names until evidence; forbid editing progress scripts; place functions into original .cpp units via assert-path strings.

## alula/CircleShootApp (Zuma Deluxe, 2003)
- Functional with asm accuracy where possible; built on open-source SexyAppFramework 1.3; MSVC 7.0 kept for diffing, builds with modern MSVC/MinGW. Fully playable. https://github.com/alula/CircleShootApp
- Lesson: when the engine layer has open source, reverse only the game layer.

## widberg/fmtk + FUELDecompilation (FUEL, 2009; VC80): delink approach
- ghidra-delinker-extension exports relocatable COFF from FUEL.exe; relinked with link.exe; functions replaced by excluding them from the delinked object. Smoke test: relink at a different base. Prioritisation via DynamoRIO drcov traces (6,895 startup fns of ~33,259). https://github.com/widberg/fmtk/wiki/Decompilation
- Pitfalls: LTCG non-standard calling conventions; Ghidra needs string-start labels, false XRefs deleted, every instruction inside a function, no duplicate symbols; CRT pieces (`.CRT$XCU`, SEH/TLS, resources, COMDAT literals, vtables/RTTI) handled separately.

## TRX / TR1X / TR2X (Tomb Raider I-III)
- Functional reimplementation in C, one engine for TR1/2/3, Windows/Linux/macOS; trx-1.12 released 2026-11-02; 31 contributors, started 2021. https://github.com/LostArtefacts/TRX

## diasurgical/devilution (Diablo, 1996)
- Faithful functional reconstruction from leaked PS1 symbols and a hidden PC debug build; VC6 SP5 + Processor Pack recommended; ~1,200 hours. DevilutionX is the cross-platform fork. https://github.com/diasurgical/devilution

## n-delic/decompdecomp (AI agent decomp app)
- Continuation of decomp.wtf (archived 2026-10-15). Agents write C per function, original compiler judges byte-equal; "autopilot" easy functions first. Windows PC Beta. "No whole game has been fully decompiled yet". https://github.com/n-delic/decompdecomp
- Lesson: "a match proves the code is correct but not that it is readable".

## Other relevant 2025-2026 Windows decomps
Index: https://recompsoftware.com/platforms/windows/
- openblack/bw1-decomp (Black & White, 2001, C++): matching, MSVC 6.0 SP4/SP5, decomp-toolkit fork splits PE into objects via `splits.txt`, objdiff, `build.sha1`; pushed 2026-11-02. Best MSVC6-C++ matching template. https://github.com/openblack/bw1-decomp
- isledecomp/racers (LEGO Racers, 1999): MSVC 6 + reccmp, ~98.6%, playable, racers-portable. https://github.com/isledecomp/racers
- ecruells/resident-evil-pc-decomp (RE1 PC, 1997): functional from Ghidra, 1716/1717 logic functions, D3D11/OpenGL wrapper, Windows + Linux (SDL2); created 2026-05-28, v1.4.1 2026-10-23 (~5 months). https://github.com/ecruells/resident-evil-pc-decomp
- OpenJKDF2: 56.8% by weight, 64-bit standalone for Windows/macOS/Linux/Android/Web. https://github.com/shinyquagsire23/OpenJKDF2
- KeeperFX: asm-blob-then-replace, fully rewritten 2022. swars: still mixes disassembly with C. Dethrace 97.66%.
- momo5502 FPS (unnamed): 14 Luna + 2 Opus 5.5 agents, ~3 months, 600-700B tokens, 99% functions present / 83% byte-exact; lessons: machine-checkable byte oracle, hashed verifier, agents cheat (inline asm, editing the verifier), worker comments are untrusted. https://momo5502.com/posts/2026-10-09-game-decompilation/
- SpaceRangersHD_decomp: Delphi, almost entirely GPT-6 Astra, byte-identical exe. homm3-decomp AI-assisted [unverified].

## Static recompilation of x86 PE
- M-HT/SR: binary -> asm -> llasm -> LLVM IR -> native (Septerra Core, Battle Isle 3); tag 2026-05-30. https://github.com/M-HT/SR
- sp00nznet/pcrecomp (created 2026-02-27): Capstone lifters for MZ/NE/LE/PE32/PE64; `win32hle` reimplements Win32 + GDI + software DirectDraw/DirectSound + Winsock + COM + registry on libc/SDL2; no guest SEH/C++ exceptions; "DirectX beyond DirectDraw 2 and DirectSound 1 is out of scope" (so D3D7 titles not covered). https://github.com/sp00nznet/pcrecomp
- Playable: Red Alert 2 Recompiled v0.1.3 (2026-10-27), ~24.9k functions, native Windows/Linux/macOS, LAN via IPXEmu https://github.com/sp00nznet/redalert2-recomp ; NFSU2 recompiled v0.14.12 (27,742 fns / 8.9M lines C) https://github.com/codepdbh/nfsu2-recompiled ; macOS ARM64 Metal fork https://github.com/TalhaMudo/NFSU2Recomp
- General lifters (RetDec, rev.ng, Reko, llvm-mctoll): no documented playable game port.

## Tool state
- Ghidra 12.2 (2026-10-22); ghidra-delinker-extension v0.9.3 (2026-10-30); reccmp v0.1.7 (2026-09-05, needs Ghidra 12.x); objdiff v3.8.3 (2026-10-19); wibo 1.2.0 (2026-07-11); decomp.me win32 compilers msvc4.0-8.0sp1.
- decomp.dev: "Sunsetting decomp.dev" disabled the public index/leaderboard and new registrations; project pages remain; admin: "AI is not the issue by itself", hostile forks and half-baked ports are. decomp.me and ghidra.decomp.dev still run. https://decomp.dev/

## Implications for a 4.8MB MSVC6 C++/STL, D3D7, Miles, Winsock target
- Closest templates: bw1-decomp (MSVC6 C++ matching, dtk+objdiff) and CMR2Decomp -> OpenCMR2 (same API stack, port from partial decomp). RE1-PC shows a functional Ghidra route to cross-platform playable in ~5 months.
- No project documented matching heavy MSVC6 Dinkumware STL/template code; treat STL-heavy TUs as a matching risk.
- Delinking (FUEL) is viable for a day-one runnable rebuild; MSVC6 non-LTCG code is friendlier; needs a pristine Ghidra DB.
- With agents: objdiff/byte-exact as a tamper-proof oracle, hashed verifier, named-agent PRs; avoid the hostile-fork pattern.
