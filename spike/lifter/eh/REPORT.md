# P1-1: guest exceptions in the lifted build

Question: can the lifted arm64 build of the original game throw, unwind and catch its own C++ and
SEH exceptions, so that the boot path survives its first throw?

Answer: yes. A lifted VC6 test program passes 14 of 14 exception cases, including hardware faults
and static TLS on a second thread. On the boot path the game's first throw has no catch, in the
original as in the lift, and the lift reports it the same way. With failing imports
(`--fail-missing`), the game reaches throws that it does catch, and each one is caught where the
original catches it. Every number below was produced by `scripts/run_eh.sh`; the logs are in
`results/`.

The guest tests and the game runs are local only. They need Docker, the VC6 libraries in `tools/`,
the pcrecomp clone in `work/`, SDL2 and macOS on arm64. The 10 unit tests in `tests/` need the
lifter-eh venv for capstone and pefile. Neither set runs in CI; wiring them in is the integrator's
call.

## Design

The game's own CRT does the work. Nothing reimplements `__CxxFrameHandler`, the catch matching or
the unwind map: they are lifted code like the rest of the game. The host supplies what Windows
supplies, plus two transfers of control that a lift cannot express.

| Piece | Where | What |
|---|---|---|
| `RaiseException` | `runtime/sfcop_eh.c` | Builds the `EXCEPTION_RECORD` and `CONTEXT` on the guest stack and walks the guest's `fs:[0]` chain, calling each lifted handler with a nested-call record around it, as the OS dispatcher does: continue-search, continue-execution, nested-exception flags, unhandled report with exit status 7 |
| `RtlUnwind` | same | Real: calls each handler above the target with `EXCEPTION_UNWINDING` (or `EXIT_UNWIND`), handles collided unwinds and unlinks the records |
| Hardware faults | `sfcop_eh_guest_fault`, called from the host's signal handler | A `SIGSEGV`/`SIGBUS` on guest memory is raised as guest `EXCEPTION_ACCESS_VIOLATION` (0xC0000005), so `catch(...)` and `__except` see it as on Windows |
| `IsBadReadPtr`, `IsBadWritePtr`, `IsBadCodePtr` | same | Ask the host's VM map (`mach_vm_region` on macOS) instead of returning "good" |
| `_JumpToContinuation` (0x00735E7B) | replaced in the lift by `run_lift_eh.py` | The CRT's jump into a catch continuation: unlinks the top record, restores `esp` from the frame and `ebp`, and longjmps to the owning function's landing pad |
| The `__except` call in `_except_handler3` (0x0073B09B) | same | The CRT *calls* the `__except` block, which never returns; the call is replaced by a transfer to the owner's landing pad |
| Landing pads | `pcrecomp` `generate.py`, patched | `RECOMP_EH_ENTER` (a `setjmp`) at entry of every function that owns a continuation. The owner of a transfer is the newest pad whose entry `esp` is at or above the frame's `ebp`, checked against `recomp_eh_table.c` (continuation → owners), which the lift writes. A continuation the table gives to a different function is fatal. One missing from the table (the static map missed it) prints a warning and resumes in the newest pad's function; no run printed one |
| Static TLS (`fs:[0x2C]`) | `host/main.c`, through the spike's TIB hook | Per-thread copy of the image's TLS template, index written, callbacks run |

`scripts/eh_frames.py` builds the static map the pads come from: every `FuncInfo` (magic
0x19930520), its try blocks and catches, the function that registers it (both the
`push ebp; mov ebp, esp; push -1; push stub` prologue and `mov eax, stub; call __EH_prolog`), each
catch funclet's continuations, every `_except_handler3` scope table, and the CRT routines the lift
replaces, found by byte signature.

## Coverage, the original game

| | Count |
|---|---|
| `FuncInfo`s with a try block | 127 |
| Try blocks | 148 |
| Catch handlers | 167: 155 with a continuation, 12 that always rethrow (none unresolved) |
| Functions that own a `FuncInfo` | 127: 91 `ebp` prologue, 36 `__EH_prolog`; none without an owner |
| SEH frames (`_except_handler3`) | 24: 27 `__except` blocks, 7 `__finally` blocks |
| Functions that need a landing pad | 139: 118 C++ owners with a continuation (9 only rethrow) + 21 SEH frames with an `__except` (3 have only `__finally`) |
| Landing pads in the lift | **139**, every function that needs one; none not lifted |
| The spike's lift, for comparison | 32 pads, from 92 frames `eh32.py` recognised |
| Continuations | 158 distinct addresses, in 160 (owner, continuation) pairs. 50 pairs lie inside the owner's lifted body, where the pad jumps to a label. The rest lie outside it, so the pad tail-calls them as entries: most were already entries in the catalogue, and the 6 that were not were added |
| CRT hooks installed | `_JumpToContinuation` 1, `_except_handler3` `__except` call 1 |

The full list, one row per function with its prologue, frame and continuations, is
`results/eh_functions_game.tsv`; the census is `results/eh_frames_game.json` and the lift's numbers
`results/lift_eh_game.json`. The game build: 262 s at `-j4`, no errors.

## Guest tests

`guest/ehtest.cpp`, built with the pinned VC6 SP4 compiler (`/GX /MT /O2`), lifted and run on the
same host as the game, so the throw, the catch matching and the unwind all go through the lifted
VC6 CRT (`results/ehtest.txt`):

| Case | Result |
|---|---|
| `throw int` caught by `catch(int)` | PASS |
| class caught by value | PASS |
| derived class caught by base reference | PASS |
| nested try: inner catch skipped, outer catch runs | PASS |
| rethrow from `catch(...)` reaches the outer catch | PASS |
| throw from inside a catch reaches the outer catch | PASS |
| unwind through two frames runs destructors in order | PASS |
| 100 throws in a loop, each caught | PASS |
| every exception object destroyed | PASS |
| `__try`/`__except` catches `RaiseException` | PASS |
| static TLS per thread, and a throw caught on a second thread | PASS |
| access violation caught by `catch(...)`, destructor run | PASS |
| access violation caught by `__except` | PASS |
| access violation records a read and a write | PASS |
| uncaught `throw int` | reported, exit status 7 |

Runtime totals for the passing run: 115 raises (4 of them hardware faults), 114 transfers into a
catch or `__except`, 116 unwinds, 0 unhandled. There is one transfer more than the test's catches
because the CRT's own `__except` in `CallCatchBlock` takes the rethrow case.

## The game

Three runs of the lifted game (`results/game_*.txt`):

| Run | What happens |
|---|---|
| `first-throw`: the OSSpecific DLL is reported missing (`--no-osspecific`) | The game throws `nException::tInvalidAccess`. It is unhandled: the chain holds three C++ frames, none with a try block covering its state, and the CRT's outermost `_except_handler3` frame. The original game has no catch for this throw either, so the lift does what the original does: the runtime prints the chain and exits with status 7 |
| `boot` | Reaches `WinMain` (0x0069A840) with no throw, then stops at `DirectInputCreateA`, the first import win32hle lacks (the spike's stop, unchanged) |
| `fail-missing`: 57 of the 75 missing imports (DirectInput, DirectDraw's clipper, the wide-char file calls, display, event log, debug help, Miles) return their documented failure value instead of stopping (`--fail-missing`) | `GetVolumeInformationW` fails and the game throws a `char*`. Seven frames unwind, the game's `catch(char*)` in `sub_0040C3BA` shows its error window and resumes at 0x0040C5D5, and the game starts its shutdown. During shutdown the logger (`sub_006FD380`) passes `NULL` to `strlen`; the fault is raised as an access violation, caught by its `catch(...)`, and resumes at 0x006FD56C. A worker thread (`sub_006FF710`) then reads address 0 outside its try blocks; that is unhandled and ends the run. `run_eh.sh` runs this twice more and compares the `[eh]` lines; the log's last line records that both matched |

So the first throw the game catches is handled as the original handles it: the frames between
unwind, and execution carries on in the catch's continuation. On the boot path itself, the first
throw has no catch in the original either. The worker thread's fault
comes after failed imports and a shutdown the game did not expect; it is a P1-2 question (what
the missing imports should return), not an exception-handling one.

## What does not work, or is not done

- **longjmp across a guest callback.** A transfer that crosses `hle_call_guest` (guest → shim →
  guest, for example a window procedure that throws) skips the shim's exit, so win32hle's lock
  depth leaks. No test or run reaches it yet.
- **Integer divide by zero** does not trap on arm64, so `EXCEPTION_INT_DIVIDE_BY_ZERO` is never
  raised. The lifted division would need a check.
- **A filter that returns continue-execution after a hardware fault** is fatal: resuming the
  faulting instruction would need the lifted code to restart mid-function.
- **`IsBadWritePtr` on `.rdata`** answers "good": win32hle maps the whole image read-write.
- **Cost of a landing pad.** macOS `setjmp` saves the signal mask, a system call on every entry to
  139 functions. `_setjmp` would avoid it if no pad ever needs the mask back.
- **Off macOS,** the `IsBad*Ptr` probe checks reads only.
- **Guest handlers run inside the host's signal handler** when the exception is a hardware fault.
  Dispatch, the filters, the destructors run by the unwind and the catch itself all run on the
  signal's alternate stack, with the signal still blocked, until the transfer longjmps out. That
  works for the tests and the game runs, but handler code that calls something that isn't
  async-signal-safe (`malloc`, `stdio`) is not safe there.
- **Read or write** is taken from the fault's syndrome register on macOS arm64 only. Elsewhere,
  every fault is reported as a read.
- **`ExceptionAddress`** is the start of the lifted function that raised, not the instruction.

## Changes needed outside this package (reported, not made)

- `toolchain/bin/msvc-run` finds the repository root as `../../..` of itself, which is wrong in a
  worktree under `.claude/worktrees/`; `run_eh.sh` sets `SFCOP_ROOT` to get past it.
- `scripts/check.sh` and CI run neither the unit tests nor the guest tests (see the top of this
  report for what they need).
- `spike/lifter/REPORT.md`'s "32 landing pads" is superseded by the 139 here.
- The pcrecomp patch, and the two `eh32.py` weaknesses in `patches/README.md`, are worth taking
  upstream.
