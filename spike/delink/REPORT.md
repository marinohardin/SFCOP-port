# P1-3 delink spike: report

Date: 2026-11-03. Target: `StarFleetOP.exe` 2.5.5.2 (sha256 checked against
`analysis/results/retail_hashes.sha256`). Tools: Ghidra 12.2 with ghidra-delinker-extension
v0.9.3, the VC6 SP4 `link.exe`, `lib.exe` and `cl.exe` from `toolchain/` (under wibo), and the
oracle's Wine 11. Every number below comes from the scripts in this folder; the small outputs
are in `results/`. How to rerun: `README.md`.

## Verdict

**The delink route works for the whole exe.**

- **Object and link.** The exe comes apart into one relocatable COFF object of 8.9 MB of
  section data, with 157,081 absolute and 227,655 PC-relative relocations. The VC6 linker
  relinks it at base 0x10000000 in about 20 s, with 0 unresolved symbols and no `/FORCE`.
- **Byte check.** Every relocation holds the moved target, and every other byte equals the
  original (`results/validate_relinked.txt`).
- **Boot.** Under a fresh Wine prefix with no CD key, the relinked exe gets as far as the
  original does: the "Starfleet Command" window, then the CD-key check. In every run that
  stayed up, the registry keys and values it read were the same 1,205 as the original's, and
  the last new one was the CD-key key.
- **Replacing a function.** One function, `tOSVersion::IsWindows98`, was then compiled from the
  SDK source with VC6 and linked in place of its delinked copy. That build validates clean and
  boots the same way.

Getting there took a preparation pass over the Ghidra program (`SfcDelinkPrep.java`). The
synthesizer only relocates what the analysis has typed correctly, and Ghidra's auto-analysis of
this exe is wrong in a few hundred places that matter. Each fix below came from a crash or a
validator finding, not from guessing, and each is logged per address in `prep_*.tsv`.

**What this does not prove.** A clean byte check means every relocation the object has is
right. It does not show that the object has every relocation it needs.

- **Unrelocated look-alike values.** 2,313 aligned data dwords still look like image addresses
  and are not relocated. `tools/candidates.py` gives each one a verdict
  (`results/candidates.tsv`, counts in `results/candidates_summary.tsv`):
  - 2,241 are printable text;
  - 32 are UTF-16 text;
  - 29 are pairs of small 16-bit values;
  - 10 sit inside runs of other non-address values;
  - 1 was reviewed by hand (`candidate_notes.tsv`).

  None is left undecided. The rules mark these values as unlikely to be addresses; they do not
  prove that no pointer is among them.
- **Rare paths.** A pointer the analysis never saw would only show up as a crash on the code
  path that uses it. The boot covers start-up only, so the oracle's replay tests (P0-5) are the
  real check.
- **Missed addresses break whatever moves.** A missed address keeps its old value, so it breaks
  as soon as its target moves. Moving to another base moves everything, and that is what the
  relinked and replaced builds test. At the original base, replacing a function still moves
  the code after it, and here also the data sections (by 0x1000, because the new object adds a
  section).

## Numbers

| What | Result | Source |
|---|---|---|
| Ranges carried | `.text` 0x401000–0x795000, `.rdata` 0x7955b8–0x835218 (the IAT before it and the import directory after it are left to the linker), `.data` + bss 0x838000–0xc83000, `.tls` 0xc83000–0xc84000; `.rsrc` goes over as its own object | `delink.sh` |
| Object | 1 COFF object, 8,911,968 bytes of section data, 17.4 MB file | `results/export_summary.txt` |
| Relocations | 157,081 `DIR32`, 227,655 `REL32`, 0 other | same |
| Instruction dwords that hold an image address but get no relocation | 40: 23 three-letter month names compared with `CMP`, and 17 round constants (the 0x400000 and 0x800000 flag masks, 10,000,000) | `results/candidates.tsv` (`code` rows) |
| Imports | 353 IAT slots mapped to symbols in the VC6 import libraries; `.def`-built libraries for DBGHELP and mss32 | `tools/imports.py` |
| Unresolved and duplicate symbols at link | 0 and 0 | `results/link_*.txt` |
| Time | Ghidra import of the shared `.gzf` 6 s, prep 36 s, export step 19 s (synthesizer 6.3 s, exporter 8.7 s), link 20–21 s | `results/timings.tsv`, `export_summary.txt`, `link_*.txt` |
| Exe size | 8,937,472 bytes against 4,812,800 | |
| Byte check, three builds | relinked at 0x10000000, same-base control at 0x400000, function replaced: 0 relocations wrong, 0 bytes changed outside relocations | `results/validate_*.txt` |
| Boot | see "Boot" | `results/boot_runs.tsv`, `regtrace_*`, `windows_*` |

The exe doubles in size because the delinked `.data` contribution includes the bss as
initialized zeros: the object has one `.data` section, and COFF cannot mark its tail as
uninitialized. Splitting the bss into its own section is a small exporter change if the size
matters. The layout otherwise matches the original:

- `.text` lands at the same RVA.
- `.rdata` sits 8 bytes later, because the new IAT is 8 bytes longer.
- `.data` and `.tls` sit at their old RVAs.

## What prep fixes

The order is the order `SfcDelinkPrep.java` runs. Counts are from the last run
(`results/prep_summary.txt`).

1. **Code the analysis never disassembled.** 124 gaps in `.text` were repaired. The 1,085 left
   are bytes after a `RET` or `JMP` that nothing reaches, data after pointer tables, and padding
   (`prep_gaps.tsv`). Without the fixes the relinked exe wrote to the old image's `.data`
   (0x8e0934) and crashed.
   - **Calls to functions marked no-return that do return.** Two had the flag cleared
     (`FUN_005050e2`, `FUN_00736254`), and the code after the 38 call sites was disassembled.
     A callee with no `RET` keeps the flag (8 sites).
   - **Instructions decoded from the middle of real ones.** They hid the real instruction
     stream. A pseudo-disassembly from the gap start runs until it meets an existing
     instruction boundary. The overlapping junk is then cleared, along with the "functions" that
     begin inside it and that nothing calls (8 removed). It is not cleared where that would
     destroy a jump table or a called function (5 sites refused, logged in each of the 4
     passes). The shared export still lists some of them, for example `FUN_004e4552` as a
     1-byte function in `ghidra/exports/functions.csv`.
   - **Switch jump tables after a `RET`.** 46 were typed: some unaligned, some after `8d 49 00`
     padding, some continuing a table the analysis had cut short. Their targets were
     disassembled.
   - **One function start after padding** (`b8 imm32 e8`, an `__EH_prolog` function).
2. **Immediate operands.**
   - **Kept or added as references:** values that are code or data addresses. That is 44,999
     references in all; 84 were added, mostly catch-block continuations loaded into a register.
     374 such addresses had never been disassembled, and became functions.
   - **Dropped:** values the analysis had taken for addresses but that are bit masks or round
     constants (13), and references to the PE header or outside every block (73).
   - **The `SBB r,r; NOT r; AND r, imm32` idiom** (VC6's conditional address) keeps its `AND`
     operand as an address. Dropping it crashed the build at 0x69a84f.
   - **Displacement references:** 166 added where the analysis had pointed the reference at an
     array element instead of the base the operand holds.
3. **Instructions outside every function.** 7,417 runs of them; 81 joined the function that
   falls into them, and 8,024 became functions. The synthesizer only relocates instructions
   inside functions.
4. **Data in `.text`.** The DirectInput keyboard format `c_dfDIKeyboard` (static data from
   `dinput.lib`, 256 entries of 16 bytes at 0x6f2860) had been disassembled as code. It is
   retyped as data with pointers to the GUID in `.rdata`. Without this, `SetDataFormat` read
   garbage.
5. **Pointers the analysis made out of short strings.** 80 dwords in `.data` hold three-letter
   tags whose values fall in `.text`, and the analysis had typed them as pointers. Examples are
   hull and weapon class tags such as `"SHT"` and `"BOX"`, locale codes such as `"ENU"`, and the
   CRT's month names. They point inside instructions, or at labels that are no function, with a
   printable value. They are turned back into bytes. Relocating them rewrote the strings, which
   the boot does not reach but the game would.
6. **Pointers the analysis never typed.** 1,883 aligned dwords in undefined data whose value
   starts a function, an instruction, a data item or undefined (bss) data became pointers;
   385 were left as values (inside instructions or data, or printable values pointing at
   non-function code).
   - **Two exceptions** keep real pointers that the rules alone would reject:
     - a value inside a string, next to other pointers (the analysis merged two strings);
     - a printable value inside a run of code pointers (the `.CRT$XCU` initializer table at
       0x838fcc).
   - **The printable-value test applies only to values that land on instructions.** Applied to
     every value, it rejected the DirectInput `rgodf` pointer at 0x7bb1fc, and the game
     crashed.
7. **Names.**
   - 770 import thunks made ordinary functions, so calls keep going through them.
   - The 353 IAT slots are named `__imp_…` after the import libraries.
   - `__tls_used` and `_WinMainCRTStartup` are named.
   - `symbols.tsv` names `tOSVersion::fInfo`, so the replacement links against it.
   - 83 labels are added at relocation targets, and 2,056 duplicate names made unique.

`overrides.tsv` (per-operand keep/drop) exists for cases the rules get wrong; none was needed.

## Problems hit, and how each was resolved

| Problem | Evidence | Resolution |
|---|---|---|
| Relocations against IAT slots had no target in the object | validator: targets outside every range | IAT left out of the object; slots named `__imp_<symbol>` so the linker's new IAT serves them; validator maps them through the link map |
| ~1,500 pointers in data never relocated | validator stale-pointer list, crash reading the old image | prep step 6 |
| Write to 0x8e0934 at start-up | Wine crash dump | prep step 1 (no-return callees, junk overlaps, jump tables) |
| An early version cleared real jump tables (0x629f14, 0x6be100) as junk | crash, `prep_gaps.tsv` | never clear a `.text` pointer or a called entry |
| Read of 0x898c28 at 0x69a84f | crash dump, disassembly | `SBB`/`AND` idiom kept (prep step 2) |
| DirectInput access violation, `edi` = 0x6f2860 | crash dump | `c_dfDIKeyboard` retyped (step 4) and the `rgodf` pointer kept (step 6) |
| Pointer into a merged string (0x8446ec → "DesireGameSpeed") | validator | exception in step 6 |
| Replaced build crashed before reading `SFC.INI` | validator check for unrelocated `CALL`s that cross the hole: `CALL atexit` at 0x414349 | an instruction decoded from the middle of a real one owned the call's bytes; junk clearing rewritten around a pseudo-disassembly that resynchronizes (step 1) |
| Strings typed as pointers by the analysis | review of the relocation table: data pointers into the middle of instructions | prep step 5 |
| `msvc-run` gets `SFCOP_ROOT` wrong inside a worktree | wibo cannot find the toolchain | scripts export `SFCOP_ROOT` (defect reported) |
| `cvtres` does not run under wibo | | `tools/rsrc2obj.py` writes `.rsrc` as a COFF object with relocated data entries |
| The replacement object pulled in iostream static initializers | duplicate symbols, extra start-up code | compiled with the SDK's `USE_PCH` switch, and only the function's section kept (`tools/extract_function.py`) |

## Replacing one function

The function replaced is `tOSVersion::IsWindows98` at 0x6f55a0.

- **Why this one:** it is 28 bytes, makes no calls, has 10 callers, and the SDK has its
  source. It reads the static `tOSVersion::fInfo` (0xc7f790), so it also proves that compiled
  code can link against data in the delinked object.
- **Removing the original:** `delink.sh export` with `EXCLUDE_FN=006f55a0` leaves its bytes
  out. The 10 callers' `REL32` relocations then target the symbol, and code after the hole
  closes up by 0x1c bytes.
- **Compiling:** `replace/build.sh` compiles the function with the SDK's Release flags and
  keeps only its own section.
- **The result:** `tools/compare_function.py` (`results/replaced_function.txt`) finds 22 of
  the 28 bytes equal. The 6 that differ are all in the two dwords that address `fInfo` fields
  (`fInfo`+8 and `fInfo`+16); they now point into `fInfo`'s new home at 0x10880790. The
  validator, given the hole, confirms:
  - every caller reaches the new function at 0x10394ff0;
  - no unrelocated `CALL` or `JMP` crosses the hole;
  - every other byte is unchanged.

## Boot

Every boot uses the spike's own prefix, `work/delink/wine/prefix`: made fresh, with an
800x600 virtual desktop and no CD key, holding a clone of the game folder. Without a CD key,
the furthest any build can get is the CD-key check.

**Evidence: window list and registry trace.** The Mac's screen was locked for the whole
session, so `oracle/shot.sh` could not capture (macOS refuses window images while locked) and
**there is no screenshot**. Two other kinds of evidence are kept instead:

- **Window list** (`windows_<n>s.txt`): the windows Wine has, listed by the oracle's input
  driver. Each running build shows a visible top-level window of class `SFC`, titled
  "Starfleet Command".
- **Registry trace** (`WINEDEBUG=+reg`, compared by `tools/regtrace.py`): every key and value
  the process opens or reads. The original and the relinked build read the same 1,205, and
  both end polling `HKLM\SOFTWARE\GameSpy\CDKeys`, the CD-key dialog waiting for input.

`run_all.sh` boots each exe three times, in turn, all with the registry trace
(`results/boot_runs.tsv`). Each rebuilt run's trace is compared with the trace of the first
original run that stayed up, run 2:

| Run | Original | Relinked at 0x10000000 | `IsWindows98` replaced |
|---|---|---|---|
| 1 | died (c0000005) | died (c0000005) | up; trace matches |
| 2 | up; the reference trace | up; trace matches | died (c0000005) |
| 3 | died (stack limits) | up; trace matches | up; trace matches |

"Up" means alive at the last sample (75 s), with the `SFC` window in Wine's window list, after
the trace has reached the CD-key key.

- **Rebuilt runs that stayed up:** all four read the same 1,205 keys and values as the
  original (`results/regtrace_*.txt`).
- **The window lists:** `results/windows_*_75s.txt`.
- **A run that died:** it stops short of the original's keys. `run_all.sh` records it as
  "differs, run died"; its diff names local audio devices, so it is not kept.
- **A run that stays up but whose trace differs:** `run_all.sh` flags it and fails.

Earlier builds of this spike died in their own ways: writes to the old image, DirectInput, the
missed `CALL` across the hole. Each is listed under "Problems hit", and none is left in the
final builds.

**Wine start-up crash.** Five of the nine runs died, two of them the original's. The original
crashes in Wine at start-up, and `oracle/BASELINE.md` analyses the crash. Each death in this
table ended in one of two ways, as `boot_runs.tsv` records:

- **"dispatch_user_callback ignoring exception c0000005":** original run 1, relinked run 1,
  replaced run 2.
- **"Exception frame is not in stack limits":** original run 3.

The rebuilt exes died only in a way the original also died. Nine runs are too few to say
whether the rate is the same.

## Route L (P0-4) and this route, compared

| | Route L (pcrecomp lift) | Delink (this spike) |
|---|---|---|
| Output | 4.2M lines of C, a native arm64 Mach-O | a Win32 exe from the original's own machine code |
| Platform | macOS natively, with every Win32 and DirectX call answered by an HLE layer | Windows or Wine only, until enough code is decompiled |
| Boot reached | WinMain and the window (0.61 s), then the first missing import (`DirectInputCreateA`) | as far as the original: the window and the CD-key check |
| What is still missing | guest C++ exceptions (1,174 throw sites), 75 imports, DirectInput, DirectSound3D, Miles | nothing for boot; the original DLLs and Wine or Windows do the work |
| Rebuild time | 6 min | about 1 min to delink (import, prep, export) and 20 s to link |
| Precision | 53-bit x87; 3DNow! lifted as no-ops | the original instructions, bit for bit |
| Replacing a function with source | possible per `sub_XXXXXXXX`, with lifted-C calling conventions | demonstrated: VC6 object linked in, byte-exact except relocations |
| Byte-matching decomp (objdiff) | not possible | the base it needs: each function can be swapped for compiled source and checked |
| Risk | runtime gaps found one at a time as boot goes deeper | pointers the analysis missed, found as crashes on the paths that use them |

**These are two tools for two jobs, not rivals.**

- **What delink gives the decompilation:** a test bed in which every function can be swapped
  for compiled VC6 code and then checked by objdiff for bytes and by the oracle for behaviour,
  with the rest of the game still the original.
- **What only Route L gives:** a native Mac build before the decompilation is done.
- **The open risk for delink:** the pointers this pass may still miss. They matter only for
  what moves.
- **Recommendation:** build the decomp workflow on this object.
  - **Keep everything at its original address** where possible: original base, and an excluded
    function's range filled with `INT3` instead of closed up, with the replacement placed in a
    section of its own. A missed pointer can then only hurt if it points into a replaced
    function. That needs a change to the export (the extension closes the hole) or a padding
    object, and is the next step for this tooling.
  - **Keep a rebased build in CI** as the detector for missed relocations: the validator and
    the replay oracle on an exe at 0x10000000.

## Limits

- **Coverage of the boot.** It only covers start-up, as far as the CD-key dialog. Entering a
  key, the menus and a match are left to the oracle, which has a prefix with the owner's key.
- **The oracle replay was not run.** The P1-3 acceptance line asks for the relinked exe to
  pass the oracle. The oracle cannot record on this Mac: P0-5 is blocked by the same Wine
  start-up crash, which kills the original after the key is entered.
- **The validator compares against what the object claims.** It cannot find a pointer the
  object never marked. It does list candidates, and `tools/candidates.py` gives every one of
  them a rule-based verdict (`results/candidates.tsv`). A rule can be wrong about a single
  value.
- **The analysis was taken as given.** The shared Ghidra project from P0-3 was used as-is,
  apart from the fixes listed. A newer analysis may need the prep rules rechecked; the
  `prep_*.tsv` logs make the difference visible.
