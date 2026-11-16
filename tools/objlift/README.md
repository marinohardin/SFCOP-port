# tools/objlift: the claimed units' VC6 objects, lifted into the native build

A function that `decomp/op/manifest.tsv` claims for VC6 (mode `vc6`, DESIGN §6.3) is compiled by
the decompilation's pinned VC6 exactly as its harness compiles it for the byte check. Its COFF object is placed in the
guest as a linker would place it, and lifted with pcrecomp the way the exe is lifted. The lifted
body replaces the exe lift's body for that address: for direct calls, for the dispatch table and
for every guest vtable slot. The source in `decomp/op/` is the original's form, with no guest-mode
constructs; the same object the harness scores exact is the one that runs. A function the
harness has not recorded exact has the mode `partial` and is skipped: the exe lift's body stays.

| File | What it does |
| --- | --- |
| `objlift.py` | The command line: claims, placement, lift, the generated files |
| `placement.py` | Sections, COMDAT selection, liveness, where each section goes, every relocation (stdlib only) |
| `liftcode.py` | The lift of placed code through pcrecomp's linear lifter |
| `compile.py` | VC6 compiles every claimed unit as the decompilation's harness does (`decomp/harness/lib/source.py`) |
| `objlift.sh` | The build step: `compile.py` on the host, then `objlift.py` in the lifter container |
| `proof.cmake`, `proof.sh` | The proof build and its run (below) |
| `fixtures/make_coff.py` | Writes the test objects (`tests/test_objlift.py`) |

## Command line

```sh
python3 tools/objlift/objlift.py --obj [UNIT=]UNIT.obj ... --manifest decomp/op/manifest.tsv \
    --symbols decomp/ghidra/exports --out DIR [--exe StarFleetOP.exe] [--lifted LIFT/out/gen] \
    [--pcrecomp DIR] [--check] [--no-lift]
```

- `--obj` once per claimed unit. The object's stem names the unit (`DynamicAllocator.obj` is
  `Common/DynamicAllocator.cpp`); `UNIT=PATH` names it when two units share a file name. The /FAs
  listing beside the object (`UNIT.asm`, as the harness writes it) says which compiled symbol each
  annotation claims; without one, the mangled name in `functions.csv` is used. A static symbol,
  such as the dynamic initializer VC6 generates from a global's definition (`_$E1`, which every
  unit with a global has), is looked up in the claimed unit's own object only.
- `--symbols` is the exe's name evidence (`functions.csv`), plus the SDK match tables the harness
  uses (`--sdk-symbols` replaces them).
- `--exe` is hash-checked against `analysis/results/retail_hashes.sha256`. It gives the evidence of
  exact functions (below), the exe's section table for `SECTION` and `SECREL` relocations, and the
  report's byte comparison.
- `--lifted` is the exe lift's `out/gen`: its `recomp_funcs.h` says which bodies exist, and its
  `recomp_dispatch.c` is merged with any new body.
- `--check` writes nothing and exits 1 when a file in `DIR` is not what this run would write.
  `--no-lift` places and reports only, with no pcrecomp.
- Exit status: 0 done, 1 `--check` found a difference, 2 an input the tool cannot honour. The
  message names the symbol or section, for example `unresolved external symbol ?Nowhere@@YAXXZ
  (referenced from missing.obj:.text#1)`.

It writes into `DIR`, each file only when its content changed, so the build recompiles only
what a change touches:

| File | Contents |
| --- | --- |
| `objlift_code_<UNIT>.c` | One per claimed unit: its lifted bodies, under the exe lift's names (`sub_006FC5A0`), with the declarations they need beyond `recomp_funcs.h`. `<UNIT>` is the unit's path without its extension as a C identifier (`Common/DynamicAllocator.cpp` gives `objlift_code_Common_DynamicAllocator.c`) |
| `objlift_funcs.h` | Every lifted body's declaration, for the table and the dispatch table |
| `objlift_table.c` | The override table (`src/app/guest/override.h`), each claim's index into it (`sfcop_objlift_index_<ADDRESS>`), and the data image |
| `objlift_dispatch.c` | The exe lift's dispatch table, with any fresh body merged in |
| `objlift_weak_NNNN.h` | One per exe-lift file `recomp_NNNN.c` (with `--lifted`): `#pragma weak` for each body of that file the objects replace; empty when it holds none |
| `objlift_report.txt` | Where every section went and why, every relocation, every judgement call |

Which exe-lift file holds a body is read from the `void sub_XXXXXXXX(void) {` lines of the
`recomp_NNNN.c` files themselves. A file an earlier run wrote that this run does not (a unit no
longer claimed, or the single `objlift_code.c` and `objlift_weak.h` of earlier versions) is
removed.

## Where each section goes

Only live sections are placed: those that hold a claimed function, and those a placed section
references, as a linker with /OPT:REF keeps them. An associative COMDAT section lives with its
parent. `.drectve`, `.debug$*` and removable sections are dropped, and so are COMDAT duplicates.
Duplicates are chosen by the section's selection (no duplicates, any, same size, exact match,
largest; each refused case is refused as a linker refuses it).

| Mode | When | Address | Lifted | Loaded |
| --- | --- | --- | --- | --- |
| claimed | holds a function the manifest claims | so that the function is at its original address | yes, replacing the exe lift's body | no |
| exe | the exe holds the contents: a symbol has an address in the exe's evidence | the exe's | no | no: references go to the exe's copy |
| exe, apart | a data section whose symbols the exe holds at unrelated addresses (the original link put each one apart) | each symbol's own | no | no |
| fresh | anything else | the next free address in the reserved range | yes, as `sub_<address>` | yes, into the data image |

The exe's evidence for an address is the name in `functions.csv` or the SDK tables. Failing that,
it is what an exact claimed function uses: the decompilation's relocation check (`decomp/harness/lib/target.py`)
reads the address a relocated field holds in the original bytes, and a function that matches
byte for byte vouches for it. That is how `??_R0PAVtAllocatedClass@@@8`, a type descriptor that no
table names, resolves to 0x0088C180. A code name with several exe addresses (copies of one
function) takes the lowest and says so in the report. A data name with several is refused.

A writable section with no exe address is refused: placed fresh, the lifted exe would keep its own
copy of that storage and the two would drift apart.

**The reserved guest range is 0x0F000000 to 0x10000000 (16 MB).** The exe's image ends at
0x00C86000, and the guest allocator hands out "anywhere" reservations from 0x10000000 up, so
nothing else maps it. A lift that needs more is refused with the size it needs.

## Relocations

Every i386 type VC6 emits, with P the field's address and S the target's:

| Type | Field becomes |
| --- | --- |
| `DIR32` | field + S |
| `REL32` | field + S - (P + 4) |
| `DIR32NB` | field + S - 0x00400000 |
| `SECTION` | field + the output section's number (the exe's sections in order, then the reserved range) |
| `SECREL` | field + S - the output section's start |
| `ABSOLUTE` | unchanged |

Any other type is refused. In the real objects so far, the placed sections use only `REL32` and
`DIR32`; `DIR32NB` appears only in `.debug$F`, which is dropped.

## Lifting

Each placed code section is one code buffer at its placed address, and pcrecomp lifts it as
`cmake/lifter/run_lift.py` lifts the exe: `generate.true_extent` with the section's end as the
limit, `linear_disassemble_function`, `eh32.resume_points` for catch landing pads,
`lift_function_linear`, and `lift32.Lifter(precise_carry=True, precise_sbb=True)`. The entries are
the section's function symbols, the claimed addresses, and any address in the section that a data
section's relocation names (EH funclets). A body that pcrecomp cannot lift, that holds an
`UNIMPLEMENTED` instruction, or that calls an address the exe lift has no body for is refused.
pcrecomp is used as pinned, with no change.

Two statements go before `RECOMP_ENTER`, where `g_cur_func` still names the caller:

- `sfcop_guest_entered(sfcop_objlift_index_<ADDRESS>)`, in a claimed body, counts the call and
  logs its first run. The index is a constant in `objlift_table.c`, not a literal, so a new
  claim, which renumbers the table, leaves every other unit's code file as it was;
- `SFCOP_GUEST_OBJECTS_LOAD()`, in every body when there is a data image, loads it the first time.

A body ends at its section's end. The exe lift's body can run on past trailing padding into the
next function, so the two can differ after a call that never returns. Three `Tiger/tpart.cpp`
bodies end in `call __CxxThrowException@8` and nop padding. Ours ends with a tail into the next
address that cannot be reached; theirs runs on into the next function's code.

### Coupling to the exe lift

Two things here follow the exe lift (`cmake/lifter/run_lift.py`) and break if it changes:

- `objlift.py` reads its generated text: `read_lifted_names` takes every `void sub_XXXXXXXX(void);`
  line of `recomp_funcs.h`; `read_dispatch` takes the `{ 0xXXXXXXXXu, sub_XXXXXXXX },` rows and the
  `const uint32_t sfcop_entry_va = 0x...u;` line of `recomp_dispatch.c`. The committed copies of
  both formats are in `fixtures/exe_lift/`, and `tests/test_objlift.py` runs both readers on them,
  so a format change fails a test.
- `liftcode.lift_sections` repeats the driver loop of `run_lift.py` (`true_extent`, linear
  disassembly, `eh32.resume_points`, `lift_function_linear` with the same `Lifter` flags); it is a
  copy, not an import. A change to that loop in `run_lift.py` has to be made here too. Nothing in
  pcrecomp itself was changed.

## The data image

Fresh sections' bytes are compiled into `objlift_table.c`. `sfcop_guest_load_objects()` reserves
the range with `hle_vm_reserve` and copies them in, once. A host calls it after mapping the exe.
Until one does, every lifted-object body loads the image on its first run (the macro above), so no
host change is needed. With no fresh section, the image is empty and the call does nothing.

## In the build

`cmake/lifter/objlift.cmake`, included from `cmake/SfcopLifter.cmake` after the exe lift, does
the following when the manifest claims anything for VC6 (option `SFCOP_OBJLIFT`, default ON):

- It runs `objlift.sh`. The step depends on `decomp/op/`, the manifest, the compiler profiles, the
  symbol evidence, the exe, the exe lift's `recomp_funcs.h` and `recomp_dispatch.c`, and these
  tools.
- It compiles the generated C into the OBJECT library `sfcop_objlift`. The per-unit file names
  are derived from the manifest at configure time, with CMake's `string(MAKE_C_IDENTIFIER)`.
- It force-includes `objlift_weak_NNNN.h` into `recomp_NNNN.c`, and into no other file, and
  makes each file depend on its own header only.
- It replaces `recomp_dispatch.c` with `objlift_dispatch.c`.
- It links the objects through `sfcop_lifted`'s interface, since an archive member is never loaded
  to replace a symbol that is already defined.
- It drops `src/app/guest/override.cpp`, guest mode's empty table, from `sfcop_src`.

With nothing claimed, or `SFCOP_OBJLIFT=OFF`, nothing is added. Guest mode (`SFCOP_GUEST_MODE`)
and the object lift are exclusive, and configure stops if both are on.

`compile.py` keeps a key beside each object (`DIR/obj/<unit>.key`): the SHA-256 of the unit's
source, its profile, every file under `decomp/op/` that is not a `.c`, a `.cpp` or the manifest, and
the compile scripts. A unit whose key did not change is not compiled again. So a change to one
unit's `.cpp` compiles that unit, lifts every unit again (the placement is global), and then
recompiles that unit's code file, the exe-lift files that hold its claimed functions, and, when
the claimed set or a fresh body changed, `objlift_table.c` and `objlift_dispatch.c`. A change to
a header compiles every unit. Fresh sections are placed one after another in the reserved range,
so a fresh section that grows or appears moves the later units' fresh bodies, and their files
change with it.

The lift runs in the lifter container (`cmake/lifter/Dockerfile`), with no network and with the
repository, the exe, the exe lift and pcrecomp mounted read-only. `OBJLIFT_PYTHON=<a Python with
capstone>` runs it on the host instead.

## Cost

| Step | Time |
| --- | --- |
| VC6 compile of one unit, container start included | 0.5 to 0.9 s |
| Lifter container start (`docker run --rm ... true`) | 0.13 to 0.15 s; with Python and capstone imported, 0.2 to 0.6 s |
| Placement and lift, `Common/DynamicAllocator.cpp` (3 functions) | 0.4 s |
| Placement and lift, the four units of the regenerated manifest (16 functions) | 0.3 s |
| The whole step for `DynamicAllocator`, `objlift.sh` | 1.7 s |

## Tests

`python3 -m pytest tests/test_objlift.py`. The fixtures are hand-built COFF objects
(`fixtures/make_coff.py`) and every expected value is worked out by hand. The tests cover:

- every relocation type, and an unknown one refused;
- an unresolved external, refused with its name;
- two functions in one section;
- data referenced from code, both the exe's and fresh;
- data the exe holds apart;
- evidence from an exact function;
- a static claim (`_$E1`) found in its own unit's object only, and refused from another's;
- the reserved range's limit;
- COMDAT any, largest, and the three refusals;
- the command line, and `--obj` split at the first `=`;
- COMDAT copies that disagree on the selection kind;
- the exe lift's output formats, on the fixtures in `fixtures/exe_lift/`;
- one code file per unit and one weak header per exe-lift file, each holding exactly its own
  bodies; a second run with the same input leaves every file's time alone; a new claim (below
  the others, so the whole table is renumbered) rewrites only its unit's file, the weak header of
  its exe-lift file, the table, the declarations and the report; a unit no longer claimed loses
  its file (with bodies written by hand, so these run everywhere);
- the compile key: one unit's source changes its key only, any header every key;
- the lifted C of a two-instruction function against `fixtures/two_instruction.c`, a golden file
  the tool produced; a test of its structure, written out by hand, runs everywhere.

The lift tests run pcrecomp in-process when capstone imports, else in the lifter container, else
they skip and print why on stderr. On the CI runner (no lift) everything runs except the two lift
tests and the retail test; those run only on a machine with pcrecomp and capstone or the container.
`SFCOP_UPDATE_GOLDEN=1` rewrites the golden file from the lift. One `retail` test compiles `Common/DynamicAllocator.cpp` with the pinned VC6 and checks
that its real object places byte for byte onto the exe.

## Proof

The three functions of `decomp/op/Common/DynamicAllocator.cpp` are written as VC6 compiles them, with
no guest-mode `PORT:` lines and no `ptr32`. Reproduce, from the repository root, with the decompilation's harness
and a build directory under the work drive:

```sh
for f in 0x006fc4d0 0x006fc5a0 0x006fc750; do sh decomp/harness/build-and-verify.sh $f; done
sh tools/objlift/proof.sh BUILD_DIR OUT_DIR [-- EXTRA CMAKE ARGS]
```

`proof.sh` builds one directory twice, configured with `tools/objlift/proof.cmake`: first
`SFCOP_OBJLIFT=OFF` (before), then `SFCOP_OBJLIFT=ON` (after). For each build it runs:

- the spike's guest test;
- `nm` and `objdump` on the claimed symbols;
- three headless boots with the clock at 15:38 and `--watchdog 60`.

Results of 2026-11-04 on develop at 1c1690d. Develop's lifted link needed
`-DCMAKE_EXE_LINKER_FLAGS=-Wl,-alias,_sfcop_spike_misc,_sfcop_hle_misc`, for a table the boot
host names differently from the DirectDraw module.

- **Harness:** each function is still 100.00% exact: 0x006fc4d0 (48 bytes), 0x006fc5a0
  (96 bytes) and 0x006fc750 (64 bytes).
- **Lift:** the three placed functions are identical to the exe byte for byte (`objlift_report.txt`).
  - Their lifted bodies equal the exe lift's, apart from the counting hook.
  - `??_R0PAVtAllocatedClass@@@8` stays the exe's (0x0088C180), so the data image is empty.
  - Only `REL32` and `DIR32` relocations are applied.
- **Guest test:** both builds print the same 25 `[test]` lines, ending `[test] all checks passed`.
  The `[path]` lines say how each address ran:
  - before: `0x006FC5A0 runs its lifted body`;
  - after: `0x006FC5A0 runs the override compiled from game/ (dispatch table entry replaced)`,
    with the overrides running 10, 5 and 7 times.
- **Direct calls:** the exe lift's `sub_006FCC10` calls `sub_006FC750` and `sub_006FC5A0` with
  `bl`. Before, those land on the lifted bodies (0x1045bf220, 0x1045bde70). After, they land on
  the lifted objects (0x100068ed8, 0x100068a60, in `sfcop_objlift`).
- **Boot:** the six boots (three per build) reach the CD-key prompt.
  - Before: 5.618, 5.592 and 5.600 s.
  - After: 5.640, 5.533 and 5.529 s.
  - Every boot exits 0 and loads `space21.mod`.
  - The after build announces its three lifted functions. The boot path calls none of them, as
    in the spike's guest build.
