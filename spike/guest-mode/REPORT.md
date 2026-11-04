# Guest-mode spike (C3, DESIGN §12.4)

**Question.** Can `game/` source be written once and compiled both ways: by VC6, byte-exact, and by
clang in guest mode into the native build, with `game/port/types.h` alone?

**Verdict: yes, with constraints.** Three functions in `game/Common/DynamicAllocator.cpp` compile
unchanged both ways:

- one plain member with a pointer parameter;
- one member that walks pointer members and a `std::vector`;
- one virtual method that is reached through a guest vtable.

The VC6 build of each is 100.00% exact. In the native build each one runs in place of its lifted
body, and the guest test's output matches the all-lifted build line for line.

"With `types.h` alone" holds only for the source file. Three more things are needed:

- a guest-mode header behind `types.h` (`game/port/guest.h`);
- one `PORT:` line per polymorphic root class and one per pure virtual;
- a generated table (`src/app/guest/override.cpp`) of guest-ABI entry points and virtual-call
  dispatchers.

The constraints below grow with the decompilation. The last section compares them with an
alternative that has none of them.

Reproduce, from the repository root:

```sh
sh spike/guest-mode/run_all.sh
```

The script runs five steps:

1. the manifest check and its tests;
2. the harness on the three functions;
3. two native builds at `-j4`;
4. the guest test in both builds;
5. three pinned-clock boots of each build.

Results go to `results/`. The two full lifted compiles take about 12 minutes; `STEPS="test boot"`
reuses the builds.

## Results

| Address | Function | VC6 harness before | After | Native build |
| --- | --- | --- | --- | --- |
| 0x006FC5A0 | `tDynamicAllocator::tAllocatorInfo::operator==(const char*) const` | no source | 100.00% exact, 96 bytes, 1 relocation | override, ran 10 times in the test |
| 0x006FC750 | `tDynamicAllocator::LockDownAllocator()` | no source | 100.00% exact, 64 bytes, 0 relocations | override, ran 5 times |
| 0x006FC4D0 | `tAllocatedClass::operator==(const type_info&)` (virtual, slot 2) | no source | 100.00% exact, 48 bytes, 4 relocations (1 unverified) | override, ran 7 times, every call through the vtable |

The "After" scores are in `results/harness_vc6.txt`. `analysis/results/matched_set.json` was not
updated, because it is not this package's file.

`game/manifest.tsv` gives the sizes 41, 93 and 54. Those come from `ghidra/exports/functions.csv`
and run to each function's last instruction. The harness compares up to the next function, which
adds the `0x90` alignment padding up to a 16-byte boundary (7, 3 and 10 bytes in the exe). That
padding gives the harness's 48, 96 and 64.

**The guest test** (`guest_test.c`, target `sfcop_guest_test`):

- It maps the exe as the boot does and builds guest objects from the game's own vtables and
  TypeDescriptors.
- It calls the three functions as lifted code calls them: `ecx` loaded, arguments on the guest
  stack.
- It also calls the lifted `tDynamicAllocator::mGetAllocatorID(const char*)` (0x006FCC10), which
  calls 0x006FC5A0 and 0x006FC750 directly.
- It makes 24 checks, each with a hand-written expected value. All pass in both builds, and the
  `[test]` lines are identical (`results/guest_test_compare.txt`).
- Its `[path]` lines say which body answered in each build.

The call counts are what the checks predict:

- 0x006FC5A0: 5 direct calls from the test, plus 2 and 3 from the lifted 0x006FCC10;
- 0x006FC750: 3 from the test, plus 2 from the lifted caller.

So the lifted code's direct calls reach the overrides.

**The direct-call binding** (`results/direct_calls.txt`):

| Build | Where the claimed symbols are | The lifted 0x006FCC10's `bl` targets |
| --- | --- | --- |
| All-lifted | `0x1045564bc` and up: the lifted bodies | the lifted bodies |
| Guest | `0x10004a564` and up: `sfcop_guest`'s objects | `0x10004a7bc <_sub_006FC750>` and `0x10004a68c <_sub_006FC5A0>`, the override entry points |

**Boot** (`results/boot_1538_*.tsv`, `tools/boot_probe.sh` with the clock at 15:38):

- Both builds reach the CD-key prompt three times out of three, at 5.54 to 5.63 s, with the same
  background (`space21.mod`).
- The guest build announces its three overrides.
- None of the three runs before the prompt: 0 calls each (`results/boot_1538_guest_overrides.txt`).
  The boot does not reach the allocator registry before the prompt; the spike did not trace where
  it does.
- So the boot shows that linking the overrides breaks nothing. It does not exercise them. The guest
  test does.

## The dispatch path

The lift calls a function three ways. All three reach the override.

1. **Direct call.** Lifted C calls `sub_006FC5A0()` by name.
   - The build force-includes a generated header into every lifted file. The header holds one line
     per claimed address: `#pragma weak sub_006FC5A0`.
   - A weak definition is not inlined into its callers, even in the same file, and the linker drops
     it in favour of a strong one.
   - `override.cpp` defines the strong `extern "C" void sub_006FC5A0(void)`.
   - `override.cpp` must be in an object library. An archive member is never pulled in to replace a
     symbol that is already defined, even weakly.
2. **Dispatch table.** `recomp_lookup(0x006FC5A0)` returns the address the lift recorded, which is
   the same symbol. It is now the override. The guest test's `[path]` lines check this.
3. **Guest vtable.** This is the path a lifted `call [edx+8]` takes when a caller compares an
   object's type.
   - Slot 2 of tAllocatedClass's vtable at 0x007BB2F8, and of every class that inherits the slot,
     holds the guest address 0x006FC4D0.
   - A lifted indirect call (`RECOMP_ICALL`) looks that address up with `recomp_lookup`, the
     dispatch table.
   - The table entry is the strong `sub_006FC4D0`.
   - That entry calls `tAllocatedClass::operator==` as compiled from `game/`.
   - **This path is simulated, not observed.** No lifted `call [edx+8]` ran in the spike: the boot
     does not reach one before the prompt. The guest test does what such a call does. It reads
     slot 2 from the object's guest vtable (`guest_test.c`, case 4) and calls that address through
     `hle_call_guest`, which looks it up with the same `recomp_lookup`.
   - The inference holds because `RECOMP_ICALL` and `hle_call_guest` resolve a guest address
     through one table, and the test checks that the table's entry is the override (`[path]`).
     The manual hook, which `RECOMP_ICALL` asks first, answers only 0x0059644A in `sfcop`
     (`src/app/boot/sfcop_game.c`), so it does not intercept these addresses.

pcrecomp's manual lookup hook (`hle_manual_hook`) alone is not enough, because it answers indirect
calls only. Direct calls, such as 0x006FCC10's calls to the other two, bypass it, and clang may
inline them.

**The entry point** (`override.h`, `Entry<>`) is the guest calling convention, generated per
function from its declared signature:

- `__thiscall` reads `this` from `g_ecx` and the arguments from the guest stack.
- It converts each argument to the parameter type: integer, `bool` (low byte), pointer or
  reference (guest address to host pointer).
- It calls the member through a member pointer.
- It writes the result to `g_eax`.
- It pops what the original's `ret n` popped: 4 × (1 + arguments) in the lift's stack, counting
  the return slot.
- `__cdecl` functions pop the return slot only.

**Calls out of decompiled code** go the other way:

- A non-virtual call to a function compiled in guest mode is an ordinary C++ call.
- A virtual call, such as `fAllocator->mAssignAllocID( i )`, compiles in guest mode to a static
  call to `tAllocator::mAssignAllocID`, because `virtual` is defined away.
- The generator therefore emits a body for that member which loads the guest vtable pointer and
  slot 2 (`virtualCall<void>( this, 2, allocID )`). That body calls the slot's address through
  `sfcop_guest_call`.
- `sfcop_guest_call` reaches the lifted `tClassAllocator<T>::mAssignAllocID` through the dispatch
  table. That is what the guest test's LockDownAllocator checks exercise.
- The generator assigns slots by VC6's rule for single inheritance: base slots first, an override
  takes its base's slot, new virtuals follow in declaration order. The rule was checked against the
  exe for tAllocatedClass (0x007BB2F8: scalar deleting destructor, `mGetAllocatorID`,
  `operator==`) and tAllocator (0x007BB2E8: slot 2 is `mAssignAllocID`, the `call [edx+8]` in
  0x006FC750).

## What `types.h` had to contain

`game/port/types.h` is the only header `game/` includes for this. Under VC6 (`_MSC_VER == 1200`) it
holds:

- the fixed-width typedefs: `int32` is `signed long`, as the SDK's mangling (`J`) says;
- `#define ptr32( T ) T*`;
- empty `SFCOP_VFPTR`;
- `#define SFCOP_PURE = 0`.

With `SFCOP_GUEST` it includes `game/port/guest.h`, and anything else is an `#error`. `guest.h`
(clang, C++17, `-fms-extensions`) holds:

- the host headers guest code may use, included first;
- the same typedefs from `<stdint.h>`;
- the guest runtime declarations (`sfcop_guest_call`, `sfcop_guest_typeid`,
  `sfcop_guest_type_descriptor`, `sfcop_guest_type_info_eq`, `sfcop_guest_fatal`), implemented in
  `src/app/guest/override.h`. `game/` declares them and nothing more;
- `sfcop::guest::ptr32<T>`, the `isPolymorphic` trait, a guest-layout `::type_info`, `typeidOf` and
  `vc6Vector<T>`;
- last, the macros `ptr32( T )`, `SFCOP_VFPTR`, `SFCOP_PURE`, `virtual`, `typeid( x )` and
  `vector`.

## Constraints

Each entry gives the construct, why it is needed, and its cost to the grind (the per-function
decompilation work of Phase 2).

1. **Fixed-width integers.**
   - Construct: `int32`, `int32u`, `int16`, `int8`... in place of `long`, and in place of `int`
     wherever the original's type is unknown.
   - Why: `long` is 64 bits on the host.
   - Cost: low and mechanical. Locals that are `int` stay `int`.
2. **Stored pointers: `ptr32( T )`**, for pointer members, pointers inside guest containers, and
   pointer globals.
   - Why: a guest object stores 4-byte guest addresses. The class converts to and from `T*`, so
     `->`, `*`, comparisons and passing to `T*` parameters compile unchanged. Locals and parameters
     stay `T*`, because they are host pointers into guest memory.
   - Why a function-like macro: VC6 has no alias templates. A `template<class T> class ptr32` under
     VC6 would change the code VC6 generates and the names it mangles.
   - Cost: one spelling per pointer member. `ptr32( T )` is unusual C++, but greppable.
   - What breaks: arithmetic on a stored pointer (`fFirst + i`) needs a conversion first. `vc6Vector`
     shows the pattern.
3. **The vtable pointer: `SFCOP_VFPTR`**, one line per polymorphic root class:
   `SFCOP_VFPTR // PORT: pointer size: ...`.
   - Why: clang would give the class an 8-byte host vtable pointer and move every member. In guest
     mode `virtual` is defined away, and the macro adds a 32-bit `sfcop_vfptr` member and the
     `isPolymorphic` marker. Under VC6 it is empty.
   - Cost: one `PORT:` line per class hierarchy.
   - Consequence: defining `virtual` away means no host header may follow it (constraint 8).
4. **Pure virtuals: `SFCOP_PURE`** in place of `= 0`, with a `PORT: guest mode: ...` line.
   - Why: clang refuses `= 0` on a member that is no longer virtual ("is not virtual and cannot be
     declared pure"). This is a hard error, not a warning.
   - The reason is not pointer size, so the line uses a fourth `PORT:` reason, "guest mode",
     proposed for CONVENTIONS §4.1 below.
   - Cost: one `PORT:` line per pure virtual. The SDK's interfaces have many.
5. **Virtual calls from decompiled code.**
   - Why: a call through a base pointer binds statically in guest mode. `tools/manifest.py` emits a
     dispatcher through the guest vtable for every virtual member that has no body in `game/`.
   - **The unsolved case.** A virtual member that does have a body in `game/` and is also called
     virtually from decompiled code: the call binds to that body, whatever the object's dynamic
     type is. It cannot also get a dispatcher, because the body and the dispatcher are the same
     symbol. The three functions do not hit this. It is the first thing that breaks (see the next
     section).
   - Cost: none per function while the case is avoided. Solving it needs the guest build to rewrite
     virtual call sites, either with a clang plugin or with a source rewrite of a copy of the unit.
     That is a tool of its own.
   - **Proposed rule** (CONVENTIONS §4.1, below): until the call-site rewrite exists, a virtual
     member with a body in `game/` is not called virtually from `game/`, and the manifest refuses
     the claim that would do it. A qualified call (`tBase::mFoo()`) is static in both modes and is
     allowed.
   - **Can `tools/manifest.py` detect it?** Partly, and not today.
     - It can refuse conservatively by name: any decompiled body that calls `mFoo(`, `->mFoo(` or
       `.mFoo(` unqualified, where some class in `game/` has a virtual `mFoo` with a body. A
       same-named non-virtual member is a false refusal, which is safe.
     - It cannot see calls written as operators, such as `*p == type` on a class whose virtual
       `operator==` has a body (0x006FC4D0 is one). Seeing those needs the types of the operands,
       which means a C++ parser, such as clang's own (libclang) on the guest compile.
     - Until then, a virtual operator with a body is called only from lifted code. A reviewer checks
       that by hand.
6. **`typeid` and `type_info`.**
   - `::type_info` is the guest's: the VC6 CRT's TypeDescriptor layout. Its `operator==` calls the
     guest's at 0x00735CD5.
   - `typeid( *this )` calls the guest's `___RTtypeid` (0x00737A93).
   - `typeid( pointer )` finds the guest TypeDescriptor by its decorated name (`.PAVtAllocatedClass@@`).
     The name is built from `__PRETTY_FUNCTION__` and looked up once.
   - Why: the original compares type identities that live in the exe. Host RTTI has no relation to
     them.
   - Cost: none per function, but the support is partial. Template types, namespaced types and
     `typeid( TypeName )` are refused at run time with `sfcop_guest_fatal`. Each would need a VC6
     name decorator.
7. **`std::vector` becomes `vc6Vector`** by macro: VC6's 16-byte layout (an allocator byte and
   three pointers).
   - Only the reading members exist.
   - Why: the vector lives in guest memory, built by lifted code.
   - Cost: one VC6-layout class per standard container the game uses: `vector`, `list`, `map`,
     `string` and more. Every member that grows the container needs the guest heap (next item).
8. **Host headers before the macros.** `guest.h` includes every host header guest code may need
   before it defines `virtual`, `typeid` and `vector`.
   - Why: a host header included after the macros would break.
   - Cost: low. The list grows with the decompilation and lives in one place.
9. **Not covered by the spike** (the three functions do not need them):
   - `new` and `delete`: they must allocate from the guest heap, through `sfcop_guest_call` to the
     CRT's `operator new`.
   - Statics and globals, including function-local statics such as `tDynamicAllocator::Instance()`'s:
     they must alias the guest VA. That needs an address per global, from an annotation the
     manifest reads.
   - C++ exceptions: the guest's are SEH-based.
   - Floating-point arguments and returns (x87 `ST0`), arguments wider than 4 bytes, and structs
     passed or returned by value (a hidden pointer). The entry point has no conversion for them, so
     a signature with one does not compile.
   - `__stdcall` and `__fastcall` (refused by the manifest).
   - Non-virtual calls to members that are not decompiled yet: they need generated thunks to the
     lifted body, which in turn need their addresses.
10. **Guest build flags:** `-fms-extensions -fno-access-control -w`, with `port/types.h`
    force-included.
    - `-fno-access-control`: the entry points name private members and nested classes. It costs
      nothing, but it hides real access errors in the guest build. VC6 still checks access.
    - `-w`: `game/` builds with warnings allowed, because the original's code warns. The three
      functions give no warnings under `-Wall -Wextra` today.
11. **Multiple bases are refused** (`tools/manifest.py`: "multiple bases are not handled").
    - Construct: `class tX : public tA, public tB`.
    - Why: under VC6 a second polymorphic base brings a second vtable pointer at a non-zero offset,
      vtable thunks that adjust `this`, and its own slot numbering. `SFCOP_VFPTR` models one 32-bit
      pointer at offset 0, and the slot model follows one chain of bases. A non-polymorphic second
      base would be layout only, but the scanner does not tell the two kinds apart yet.
    - Cost: every class with multiple inheritance stays unclaimable until the generator is extended.
      The extension needs one `SFCOP_VFPTR` per polymorphic base subobject, entry points that adjust
      `this`, and per-subobject slot tables, each checked against the exe's vtables. It is a
      one-time cost, but a medium one.
12. **Overloaded virtuals are refused** ("overloaded virtual ...: VC6 slot order not modelled").
    - Construct: two virtual members with the same name and different parameters.
    - Why: VC6 does not necessarily give overloads slots in declaration order, and the spike checked
      the rule only on vtables without overloads.
    - Cost: low once one vtable with overloads in the exe has been read and the rule written into
      `vtable_slots`. Until then, such a class's virtuals cannot be dispatched.
    - Related refusal: a base class with virtual members but no `// VTABLE:` annotation. It costs
      one annotation line per class.

## What breaks first beyond three functions

Roughly in the order the decompilation would hit them, starting with the rest of
`DynamicAllocator.cpp`:

1. **`tDynamicAllocator::Instance()`.** It returns the one registry object. Wherever that object
   lives (a static), guest-mode code must reach it at its guest address, and nothing maps a static
   today. This is constraint 9.
2. **Whatever creates the registry's vector.** The registry holds the vector by pointer, so the
   code that creates it calls `new`, and the guest heap is needed.
3. **The first decompiled override of a virtual that decompiled code also calls virtually.** This is
   constraint 5's unsolved case. It breaks silently, because the call binds statically, so the rule
   in constraint 5 has to hold before the case is hit. The by-name check would catch most of these.
   Operator calls need a parser.
4. **The first function that throws or catches.**

## Build

The spike prototypes the `SFCOP_GUEST_SOURCES` option in `spike/guest-mode/guest.cmake`, loaded
with `-DCMAKE_PROJECT_INCLUDE=...`, because `CMakeLists.txt` and `cmake/` are hotspot files. To fold
it into the build:

- `option(SFCOP_GUEST_SOURCES)` and the body of `_sfcop_guest_setup` move into
  `cmake/SfcopLifter.cmake`.
- `override.cpp` moves from `sfcop_src` into the `sfcop_guest` object library unconditionally. It
  builds the empty table without the option.
- The `-include weak_claimed.h` should go only on the lifted files that define a claimed function,
  not on every lifted file. Callers in other files cannot inline across files without LTO. Today a
  change to the manifest recompiles every lifted file: about 6 minutes at `-j4`.
- The lifter could instead emit the claimed functions' definitions weak from the manifest itself.
  DESIGN §6.3 says "dropped"; weak gives the same result without regenerating the lift.

## The alternative: compile 32-bit, then lift the object

The native build could take the VC6 object, or a clang `-target i686-pc-windows-msvc` object, of
each decompiled unit and lift it as it lifts the exe. Guest mode would then not exist:

- pointers, vtables, `typeid`, the STL, `new` and exceptions are the guest's own;
- constraints 1 to 10 disappear;
- the object VC6 builds is already byte-exact, so the native build would run exactly the code the
  harness verified.

The costs:

- a lifter for COFF objects with relocations;
- decompiled code that is still lifted C in the native build until the guest mode is retired;
- debugging in the lifted form rather than in the source.

This spike shows the source route works for simple code. Its runtime grows with every construct
the game uses (containers, heap, statics, exceptions, the virtual-call rewrite).

**For the decision:** compile each decompiled unit with VC6 and lift the object, instead of
compiling the source in guest mode.

1. **Benefit, fidelity.** The native build runs the very object the harness scored exact. There is
   no second compile of the source and no guest runtime that could behave differently.
2. **Benefit, scope.** Constraints 1 to 12 disappear, and so does the growing runtime: containers,
   heap, statics, exceptions, the virtual-call rewrite, multiple bases. No `PORT:` lines are needed
   for guest mode.
3. **Cost, tooling.** pcrecomp must lift COFF objects with relocations and resolve them against the
   exe's symbols. That is a one-time tool, but untested here.
4. **Cost, native code.** Decompiled code stays lifted C, not C++, in the native build until the
   guest mode is retired. Debugging and profiling stay in the lifted form, and there is no gain
   from clang's optimiser on real types.
5. **Cost, build.** Every native build needs the VC6 container (Docker) to compile `game/`, on
   every developer machine and on the runner, and the native build cannot be made without it.
   Neither route needs a function to be exact before it ships.

## Documentation changes this verdict implies

These are for the integrator; `docs/` is not this package's.

- **DESIGN §7.2.**
  - Replace "`ptr32<T>` from `game/port/types.h`, which is a plain `T*` in the VC6 build" with
    "`ptr32( T )` from `game/port/types.h`, a macro that is a plain `T*` in the VC6 build and a
    32-bit guest pointer class in guest mode".
  - Replace the sentence from "Whether `game/` source can be written once" to "into the guest build"
    with: "The guest-mode spike (§12.4) compiled three matched functions, one of them a virtual
    method, both ways from one source: `types.h` selects `port/guest.h` in guest mode, a polymorphic
    class carries an `SFCOP_VFPTR` line, and `tools/manifest.py` generates the guest-ABI entry points
    and the virtual-call dispatchers (`spike/guest-mode/REPORT.md`, with the constructs guest mode
    does not cover yet)."
- **DESIGN §6.3.** Replace "a claimed function's lifted body is dropped from the native build" with
  "a claimed function's lifted body is made weak in the native build, and the override table's
  entry point replaces it for direct calls, the dispatch table and the guest vtables".
- **DESIGN §12.4**, status column: "Answered 2026-11-03: yes, with the constraints in
  `spike/guest-mode/REPORT.md`; the object-lifting alternative is to be decided before Phase 2."
- **CONVENTIONS §4.1.** Replace the line that begins "Pointer-typed members and `long`" with these
  rules:
  - Pointer-typed members are `ptr32( T )`, a macro because VC6 has no alias templates.
  - `long` members are `int32` or `int32u`.
  - Every polymorphic root class's first line is `SFCOP_VFPTR // PORT: pointer size: ...`.
  - A pure virtual is `SFCOP_PURE` with a `PORT: guest mode: ...` line, never `= 0`.
  - Add a fourth allowed `PORT:` reason, "guest mode": a construct clang rejects only because guest
    mode defines a keyword away.
  - Until virtual call sites are rewritten in the guest build, a virtual member with a body in
    `game/` is not called virtually from `game/`, whether by name or by operator; the manifest
    refuses such a claim where it can see it. A qualified call (`tBase::mFoo()`) is allowed.
  - The guest build compiles `game/` with `-fms-extensions -fno-access-control -w` and
    `port/types.h` force-included.
  - A polymorphic class is preceded by `// VTABLE: STARFLEETOP 0x...`, its vtable's address.
  - Host headers are included before any `game/` header, or added to `port/guest.h`'s list.
  - `game/port/guest.h` is exempt from the "only what VC6 compiles" rule, because VC6 never reads
    it.

## Files

| Path | What |
| --- | --- |
| `game/port/types.h`, `game/port/guest.h` | the two compile modes |
| `game/Common/DynamicAllocator.h`, `.cpp` | the three functions and the classes they need |
| `game/manifest.tsv` | generated: address, size, symbol, source, modes |
| `tools/manifest.py`, `tests/test_manifest.py` | manifest and override-table generator, its tests |
| `src/app/guest/override.h`, `override.cpp` | the entry-point machinery and guest runtime; the generated table |
| `spike/guest-mode/guest.cmake` | the `SFCOP_GUEST_SOURCES` prototype and `sfcop_guest_test` |
| `spike/guest-mode/guest_test.c` | the guest test |
| `spike/guest-mode/run_all.sh` | reproduces `results/` |
