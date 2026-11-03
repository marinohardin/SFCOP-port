# Route decision (P1-4)

Written 2026-11-03 by the integrator from the three Phase 1 spikes. It closes the question
`PROPOSAL.md` §4 left open: lift the whole exe and replace it function by function (Route L), or
delink it into relocatable objects and relink. The sources for every number are the spike
reports named below; nothing here was measured again.

## Decision

**Both, with one job each.**

1. **Route L is the native client.** The lifted arm64 build is the only path to a native
   Apple Silicon game before the decompilation is complete, and the two blockers the Phase 0
   spike named are gone (§2). Phase 2 builds the platform layer against it.
2. **The delinked object is the decompilation test bed.** Every function the grind rewrites is
   compiled with the pinned VC6 SP4 compiler, byte-checked against the original with objdiff,
   linked into the delinked exe in place of the original copy, and run through the replay
   oracle with the rest of the game still original. That is the "free oracle" `PROPOSAL.md`
   wanted, and it is what makes a function count as **exact** in `scripts/progress.py`.
3. **One source tree feeds both.** A decompiled function is written once, in VC6-compatible
   C++ under `game/`. The byte check compiles it with VC6; the native build compiles the same
   file with clang and drops it in where the lifted body was. A function is **functional** when
   it passes the oracle in the delinked exe and the native build; **exact** when objdiff also
   says so. The 32-bit Windows exe that falls out of the delink track is the Windows port
   until the native build is complete, and the way to run the original DLLs against new code.

## Evidence

| Question from Phase 0 | Spike | Result |
| --- | --- | --- |
| Can guest C++ exceptions work in the lifted build? | P1-1, `spike/lifter/eh/REPORT.md` | Yes. A Windows-style dispatcher on the lifted CRT handlers: 139 of 139 landing pads, all 127 catching functions covered, 13 of 13 guest tests pass; with imports forced to fail, the game's own `catch` handlers run and resume in their continuations. On the plain boot path the first throw is uncaught, as in the original. |
| Can the 75 missing imports be answered well enough to boot? | P1-2, `spike/lifter/hle/REPORT.md` | Yes. DirectInput keyboard, Miles sample playback, DirectSound3D (stored, not applied), the wide-char KERNEL32 calls and the Winsock gaps are filled; the lifted game boots natively on arm64 macOS to the CD-key prompt in about 10 s and idles there without a fault; 210 self-test checks pass; 66 to 68 frames per second shown in a window. |
| Can the exe be delinked and relinked without `/FORCE`? | P1-3, `spike/delink/REPORT.md` | Yes. One COFF object (157,081 `DIR32` and 227,655 `REL32` relocations) links with VC6 in about 20 s with 0 unresolved symbols; relinked at another base, 0 relocations wrong and 0 bytes changed outside relocations; boots in Wine as far as the original and its registry trace equals the original's (1,205 keys and values). |
| Can one function be replaced with compiled source? | P1-3 | Yes. `tOSVersion::IsWindows98` compiled from the SDK source links in place of the delinked copy, byte-identical except its two relocated operands, and all 10 callers reach it. |
| Is there a metric and a gate? | P1-6, `analysis/results/progress/PROGRESS.md` | Yes. The baseline: 0% exact, 0% functional, 92.52% of `.text` lifted (99.92% of function bytes; 100 small functions, 0.07%, are still original x86; the rest of `.text` is not function bytes); 9.81% of functions carry a real symbol; the gate refuses a regression keyed by original address. |
| Is there a build-and-verify loop? | P1-5, `harness/` | Yes. A round trip through VC6 and objdiff reaches 100% and a deliberate one-line break is refused at 97.93%. |

What the spikes did **not** show, in their own words:

- P1-2 boots to the CD-key prompt, not the menu. The menu needs a key: the game's stored
  registry value supplied to the run by an `oracle/` script that never prints it, or typed input
  through DirectInput. That, and the menus, are the next milestone; DirectSound3D stores values
  without applying them.
- P1-3 covers start-up only; the validator cannot find a pointer the analysis never marked
  (2,313 candidates listed, the 72 non-text ones reviewed by hand). A missed pointer only
  matters for code that moves.
- Neither route has a replay oracle recording on this Mac yet: the Wine build crashes a few
  seconds after launch in most boots after the CD key (`oracle/BASELINE.md`), so the trace is
  recorded on a Windows PC or CrossOver, or in the native build once it reaches the menu.

## What changes in the plan

These are reported here for the integrator's hotspot pull request against `PROPOSAL.md` and
`docs/`; this memo does not edit them.

- **Phase 2 (`PROPOSAL.md` §5.3) splits into two tracks that run at once.** Track A, platform:
  turn the P1-2 HLE modules and the P1-1 runtime into `src/` (SDL3 window, input, audio;
  Direct3D 3 and DirectDraw 4 over a modern renderer), reach the menu, then a match. Track B,
  decompilation: the grind, in the harness, against the delinked object, starting with the
  boot path and the SDK-matched functions whose bytes are already known.
- **The delink export keeps original addresses.** As P1-3 recommends: original base, an
  excluded function's range filled with `INT3` rather than closed up, the replacement in a
  section of its own, and a rebased build kept in CI as the detector for missed relocations.
  This is the first Track B tooling task.
- **The harness's byte check uses the delinked object as its base**, and `scripts/progress.py`
  reads the harness's matched set: a function is exact only when objdiff says so against the
  original bytes at its original address.
- **The lifter's catalogue and runtime are patched, not forked.** P1-1 and P1-2 carry
  upstreamable pcrecomp patches (landing pads outside the body, Winsock calls, the empty-name
  object fix, a manual hook). They stay as patches under `spike/lifter/*/patches/` until the
  owner decides whether anything is offered upstream, which is a public step.

## Risks carried forward

| Risk | Where it bites | Mitigation |
| --- | --- | --- |
| A longjmp across a guest callback leaks win32hle's lock depth (P1-1) | Any throw through a Win32 callback in the native build | Fix in the runtime before Track A reaches the menu; P1-1 lists it as not done |
| x87 lifted as 53-bit doubles; 3DNow!/MMX/SSE paths unreached on this host (P0-4, P1-2) | Physics and replay determinism between native and original | The replay oracle will show it; keep the x87 question open until a match replays |
| Missed pointers in the delinked object (P1-3) | A replaced function that something still reaches by its old address | Original addresses with `INT3` holes; rebased build in CI; the validator's candidate list |
| No oracle recording on the Mac (P0-5) | Nothing is "functional" until a trace exists | Record on a Windows PC or CrossOver; or from the native build at the menu |
| The uncapped menu loop (P1-2) | Battery and heat on the laptop | The renderer package owns a frame cap |

## Reproduce

Each row of the evidence table is reproduced by the one command its task directory's `README.md`
names: `spike/lifter/eh/scripts/run_eh.sh`, `spike/lifter/hle/scripts/run_all.sh`,
`spike/delink/run_all.sh`, `python3 scripts/progress.py`, and the harness's own README.
