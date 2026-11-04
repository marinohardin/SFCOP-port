# src/hle: the Win32 surface

The game calls Windows and DirectX through a fixed set of entry points: 353 imports from 13 DLLs and
the methods of 19 COM interfaces it obtains from them. That set is the **surface**, the contract between
`game/` and `src/` (DESIGN §6.1). The game side never calls outside it. The platform side implements
all of it, in `src/hle/<module>/` on the components of `src/platform/` (Wave 1).

| File | What |
|---|---|
| `surface.tsv` | One row per entry point: 353 imports and 426 COM methods |
| `com/<Interface>.h` | The vtable of each COM interface, in slot order, with its IID |
| `com/ComBase.h`, `com/*Types.h` | The scalar types, `IUnknown`, and per family the forward declarations and callback types |
| `gen_surface.py` | Writes `surface.tsv` from the exe's import table and the `com/` headers |

## Regenerate

```bash
python3 src/hle/gen_surface.py
```

`--check` exits 1 if `surface.tsv` is not what the generator would write. `--libs DIR` also checks the
signature table against a directory of Win32 import libraries (`KERNEL32.LIB` and the others). The
exe is found the way the harness finds it (`harness/lib/paths.py`; `SFCOP_REFERENCE` or `HARNESS_EXE`
override it), and its sha256 is checked against `analysis/results/retail_hashes.sha256` first.
`tests/test_surface.py` runs the same comparison where the exe is present.

## Columns

| Column | Content |
|---|---|
| `module` | The DLL in upper case (`KERNEL32.DLL`), or the COM interface (`IDirectDraw4`) |
| `name` | The import name exactly as the exe binds it (MSS32's are decorated, `_AIL_startup@0`); for an import by ordinal, the export's name; for COM, the method |
| `ordinal_or_slot` | The ordinal for an import by ordinal (WSOCK32 and DSOUND), `-` for an import by name; the 0-based vtable slot for COM |
| `convention` | `stdcall`, `cdecl`, `thiscall` or `varargs` (cdecl with a variable argument list) |
| `stack_bytes` | Bytes the callee pops: 0 for `cdecl` and `varargs`; for COM it counts `this` |
| `status` | Below |
| `evidence` | For an import, a call site: the first `call [slot]`, else the first call of a `jmp [slot]` thunk, else the first tail jump, else the first register load. For COM, a call site attributed to that interface and slot, or `-` when none has been |
| `notes` | What answers the entry today, and anything an implementer needs |

## Status

| Status | Meaning |
|---|---|
| `implemented` | Implemented in `src/hle/<module>/` with its doctests (CONVENTIONS §8) |
| `stub` | Answered in `src/hle/<module>/` with a fixed result, because the game's use needs no more; the notes say which |
| `implemented-spike` | Not yet in `src/hle/`, but the lifter spike's module (pcrecomp's win32hle or the P1-2 modules in `spike/lifter/hle/`) performs the call's documented effect |
| `stub-spike` | The spike's module returns a fixed result instead of performing the effect; the notes give the result |
| `unused` | Present in the interface or import table, but no call site reaches it |
| `todo` | Called, and nothing answers it yet |

**A row's status, evidence and notes change only in the commit that implements the entry** (CONVENTIONS §8),
by hand. The generator keeps them; it rewrites only what the exe and the headers determine. A new
import or method appears as `todo`.

The `-spike` statuses and their notes were seeded once, on 2026-11-03, and are not regenerated. A
scratch script outside the repository matched each row to the spike's shim for it
(`spike/lifter/results/imports_vs_win32hle.tsv`, `spike/lifter/hle/results/imports_previously_missing.tsv`
and the shims' source). Every row the spike's stubs table in `spike/lifter/hle/README.md` names, and
every shim short enough to return only a constant or an error, was then judged by hand against the
rule above. DirectPlay rows come from `analysis/netcode/directplay.md`,
which lists every DirectPlay call in the game: a method it lists is `todo`, any other is `unused`.
"Called on the boot to the CD-key prompt" comes from `spike/lifter/hle/results/run_headless_import_counts.txt`.
From now on a status changes only with an implementation, as above.

## What the surface contains, and why

- **Imports.** All 353 entries of the exe's import table. Every one is named by at least one instruction
  in the code, so none is `unused`; whether each call site is reachable at run time is not settled by
  this static scan.
- **COM interfaces.** Those the exe can obtain, from the IIDs and CLSIDs in its `.rdata` and the
  objects its import calls return:
  - DirectDraw: `IDirectDraw` (from `DirectDrawCreate`), `IDirectDraw2` and `IDirectDraw4` (queried),
    `IDirectDrawSurface3` (queried) and `IDirectDrawSurface4`, `IDirectDrawClipper`, `IDirectDrawPalette`.
  - Direct3D: `IDirect3D3`, `IDirect3DDevice3`, `IDirect3DTexture2` (queried from a surface),
    `IDirect3DViewport3`.
  - DirectInput: `IDirectInputA` and `IDirectInputDeviceA`. The game calls `DirectInputCreateA` with
    version 0x0700 and holds the interface it returns; it never queries for the 2A or 7A interfaces,
    whose IIDs are not in the exe.
  - DirectSound: `IDirectSound` (from `DirectSoundCreate`, ordinal 1), `IDirectSoundBuffer`,
    `IDirectSound3DBuffer`, `IDirectSound3DListener`.
  - DirectPlay: `IDirectPlay4A` and `IDirectPlayLobby3A`, created with `CoCreateInstance`.
- **Miles.** The 26 `AIL_*` imports of MSS32 are rows like any other import.

COM evidence is incomplete for DirectDraw, Direct3D and DirectSound: the spike's census attributes a
call site to an interface only when the object field it loads fits one interface
(`spike/lifter/scripts/dx_inventory.py`), and leaves the rest unattributed. So `-` there means "not yet
attributed", not "unused". DirectPlay's inventory is complete, and its uncalled methods are `unused`.

## Sources

| Entry | Source of the convention, stack size and order |
|---|---|
| Imports other than MSS32 | The decorated names (`_CreateFileA@28`, `_wsprintfA`) and ordinals in the Visual C++ 6 (VC98) Win32 import libraries; DBGHELP's from `IMAGEHLP.LIB`, which exports the same functions. Written into `SIGNATURES` in `gen_surface.py` |
| MSS32 imports | The decorated import names in the exe |
| `IDirectDraw`, `IDirectDraw2`, `IDirectDraw4`, `IDirectDrawSurface3`, `IDirectDrawSurface4`, `IDirectDrawClipper`, `IDirectDrawPalette` | Wine 9.0 `include/ddraw.h` |
| `IDirect3D3`, `IDirect3DDevice3`, `IDirect3DTexture2`, `IDirect3DViewport3` | Wine 9.0 `include/d3d.h` |
| `IDirectInputA`, `IDirectInputDeviceA` | Wine 9.0 `include/dinput.h` |
| `IDirectSound`, `IDirectSoundBuffer`, `IDirectSound3DBuffer`, `IDirectSound3DListener` | Wine 9.0 `include/dsound.h` |
| `IDirectPlay4A` | Wine 9.0 `include/dplay.h` |
| `IDirectPlayLobby3A` | Wine 9.0 `include/dplobby.h` |

The headers are written in this project's form; only the order, the signatures and the GUID values
come from the public headers. The game's own calls confirm the order independently where they can be
read: the method-name error strings next to every DirectPlay call, and the slots the spike's census
found (`tests/test_surface.py` checks 65 of them).

## Implementing a module (Wave 1)

A module lives in `src/hle/<module>/` (`ddraw4`, `d3d3`, `dinput`, `miles`, `ds3d`, `kernel`,
`registry`, `winsock`, `dplay`) and has two layers, so that its logic is tested without a guest:

1. **The module itself**, plain C++ in `namespace sfcop::hle::<module>`: the Win32 or COM
   semantics as functions and classes that take explicit arguments and work only through the
   component interfaces of `src/platform/` (`Gpu`, `Window`, `Input`, `Audio`, `Store`,
   `Transport`, `Clock`, `Threads`). Objects that mirror COM lifetimes keep a reference count and
   say so (CONVENTIONS §4.2). Every entry has a doctest through a component mock: the call as the
   game makes it (arguments, return, out-parameters) and its error path (CONVENTIONS §8).
2. **The guest shims**, `<Module>Guest.c` or `<Module>Guest.cpp` in the same directory: one shim
   per surface row, in pcrecomp's `win32hle_shim` form (`runtime/win32hle/win32hle.h`; the
   temporary boot modules in `src/app/boot/` are worked examples). A shim reads its arguments from
   the guest stack, converts guest addresses and strings, calls the module, writes out-parameters
   back into guest memory and purges the stack it was given (`RET(value, argc)`, with `argc` from
   the row's `stack_bytes` / 4). A COM object's guest vtable is built from its method shims with
   `hle_com_vtable()`. The file exports one table, `extern const win32hle_shim
   sfcop_hle_<module>[]`, NUL-terminated, where `<module>` is the file's stem before `Guest`,
   lowercased (`Ddraw4Guest.c` exports `sfcop_hle_ddraw4[]`); it exports nothing else. The build
   generates the registration of every such table it finds (`cmake/sfcop_hle_modules.c.in`). The
   guest runtime (`src/app/guest/`, B6) calls it before anything else, so a module answers every
   name it has. The temporary boot host calls it after the spike's tables, because the spike's
   shims are the ones that host feeds (SDL2 keyboard and audio, software DirectDraw) while a module
   runs on components only B6's host constructs; until B6, a module's proof against the running
   game is through its own tests and harness, and the boot client keeps working. `*Guest.*` files are compiled only with the lift
   (`cmake/SfcopSources.cmake`), never into `sfcop_src` or the tests. If the lift's headers do not
   compile as C++, write the shim file in C and call the module through `extern "C"` functions it
   declares for that purpose.

**The surface rows.** When a package implements an entry it changes that row's `status`
(`implemented` or `stub`), `evidence` and `notes` in `surface.tsv`, in the same commit; it never
adds, removes or reorders rows, and it touches only rows whose `module` is one of its own. Because
the TSV is a hotspot, a Wave 1 pull request that edits its rows carries the `contract-change`
label, and `python3 src/hle/gen_surface.py --check` must still pass.

**Handle types.** A module that gives the game handles through win32hle's handle table claims a
type number here, so that two modules never share one: `kernel` 24 (files) and 25 (find
handles); `registry` claims none (keys are the module's own guest integers). The next module
takes the next free number and adds its line in the same pull request as its shims.

## Writing a COM layout

`gen_surface.py` reads the headers, so their form is strict:

- One interface per `<Interface>.h`, the file named after it, including its family's `*Types.h`.
- `typedef struct <Interface>Vtbl {` and, between `// clang-format off` and `// clang-format on`,
  one method per line, never wrapped:
  `/* NN */ HRESULT (SFCOP_STDCALL *Name)(<Interface> *self, ...);` with NN the slot from 0.
- Every argument is 4 bytes on the guest stack. A by-value 8-byte argument (a double, a 64-bit
  integer, a structure) is refused; add its size to the parser first.
- The IID as `static const GUID kIID_<Interface> = {...};`, and `SFCOP_COM_ASSERT_SLOTS(<Interface>Vtbl, N);`,
  which holds at 4 bytes a slot on the i386 guest target.

On a 64-bit host the structs give slot order and signatures only. How the game's 4-byte vtables reach
host code is the guest-mode question, DESIGN §12.4.
