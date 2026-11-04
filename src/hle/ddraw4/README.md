# ddraw4: DirectDraw 4 on Gpu and Window

The DirectDraw the game draws its 2D screens with, and the surfaces Direct3D (`../d3d3/`) draws
into, built on the `Gpu` and `Window` components (`src/platform/`). It serves `DDRAW.DLL`'s three
exports and `IDirectDraw`, `IDirectDraw2`, `IDirectDraw4`, `IDirectDrawSurface3`,
`IDirectDrawSurface4`, `IDirectDrawPalette` and `IDirectDrawClipper`; `../surface.tsv` says what
each entry does.

## Layers

| Files | What |
|---|---|
| `Ddraw4Defs.*` | The DirectDraw constants and structures the module uses, without their `dwSize` |
| `ComObject.*` | The reference count every object shows the game, shared by all interfaces of an object |
| `DirectDraw.*` | Display modes, cooperative level, capabilities, and the factory for the rest |
| `Surface.*` | A surface: a host copy of its pixels (`PixelMemory.h`) and a Gpu texture made on first use |
| `Palette.*`, `Clipper.*` | 8-bit palettes on Gpu palettes; clip lists and windows |
| `*Test.cpp`, `Ddraw4TestRig.h` | The doctests, through `GpuMock` and `WindowMock` |
| `GuestViews.*` | The game's COM objects for the host objects, shared with `../d3d3/`; plain C++, tested |
| `GuestAccess.h`, `Ddraw4Guest.cpp` | The guest shims (compiled only with the lift) |
| `tools/framediff.py` | Percent of differing pixels between two runs' frames (BMP files; `--self-test` builds its own frames) |
| `tools/check_tables.py` | Compares the guest shim tables of both modules with `../surface.tsv` |

## How a surface keeps its pixels

A surface has two copies: the host copy the game locks, in memory the game can address, and a
Gpu texture. Each knows whether it is current. `Lock` makes the host copy current (reading the
texture back only when the Gpu drew into it since), and `Unlock` uploads the area the lock could
have changed. `Blt` and `BltFast` run on the Gpu when both surfaces have textures and on the host
copy otherwise. A colour fill also fills a host copy that was current, so that the 2D screens,
which mostly fill, lock and blit, never read back. The primary is shown by `Flip`, which copies the
first back buffer to the front (the back buffer keeps its content) and presents it, or, for a
primary without back buffers, after each write.

## The guest layer

`Ddraw4Guest.cpp` exports `sfcop_hle_ddraw4[]` with the three `DDRAW.DLL` exports; the COM
methods are vtables built with `hle_com_vtable()` from tables in slot order, each entry carrying
its row's stack bytes (`shim<Method, Bytes>`). The method names in those tables carry a `sfcop!`
prefix so that they never resolve to the boot platform's shims of the same interface.

- A COM object the game sees is a guest block, its vtable and a count word, per host object and
  interface (`GuestViews`). All interfaces of an object share the host object's count; the
  blocks are freed when the host object is destroyed (`ComObject::SetDestroyHook`).
- Every structure is read and written field by field at its documented offsets
  (`GuestAccess.h`), up to the `dwSize` the game gave where it has one.
- Surface pixels live in guest memory (`hle_alloc`), so `Lock` hands the game a pointer.
- `GetDC` hands out a DC drawing into the host copy through the boot platform's GDI
  (`hle_gdi_surface_dc`).
- QueryInterface for an interface these objects do not serve (`IDirect3D3`, `IDirect3DTexture2`)
  goes to the hook `../d3d3/D3d3Guest.cpp` installs.

**The Gpu and Window.** `DirectDrawCreate` uses the Gpu and Window the guest runtime sets with
`SetGuestHost()` (`GuestViews.h`). Until it does, the three exports hand the call to the boot
platform's DirectDraw (TEMPORARY, until the guest runtime replaces the boot platform), so
registering this table changes nothing on its own.

## What it does not do

- Overlays, `DuplicateSurface`, `BltBatch`, `SetSurfaceDesc`: `DDERR_UNSUPPORTED`.
- Destination colour keys, raster operations other than `SRCCOPY`, `BLACKNESS` and `WHITENESS`,
  and `DDBLTFX` effects: `DDERR_UNSUPPORTED`. A colour key range uses its low value.
- Pixel formats: 8-bit palettized, RGB565, ARGB1555 (X1R5G5B5 is drawn as it), XRGB8888 and
  ARGB8888, 16-bit z-buffers. The Gpu has no ARGB4444.
- Vertical blank: nothing waits; the fixed answers are in `../surface.tsv`.

## Tests

`sfcop_tests` (the doctests) and `sh scripts/check.sh`. The shims are compiled only with the lift;
`python3 tools/check_tables.py` compares their tables with `../surface.tsv` (names, slot order,
stack bytes) and `tests/test_hle_ddraw_tables.py` runs it, with `framediff.py --self-test`: today
260 entries match their 260 rows (257 vtable slots of 11 interfaces and the 3 DDRAW.DLL exports).

## Proof run

Pending B1 (the SDL backends) and B6 (the guest runtime). The temporary boot host registers the
spike's tables ahead of the modules, so it never reaches these tables; the frame diff runs through
the guest runtime's host at gate G1. With both, the proof run is:

    tools/boot_probe.sh --shots A --shot-every 2 --headless --watchdog 60      # with these tables
    tools/boot_probe.sh --shots B --shot-every 2 --headless --watchdog 60      # without
    python3 src/hle/ddraw4/tools/framediff.py A B

The boot host's `--shots` reads the boot platform's primary, so the run with these tables needs
a capture of this module's frames first (`DirectDraw::DisplayTexture`, read back).
