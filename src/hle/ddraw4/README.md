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
texture back only when the Gpu drew into it since), and `Unlock` marks the area the lock could
have changed. Nothing is uploaded there: the marked areas merge into one rectangle, uploaded once
at the surface's next Gpu use (a bind, a draw, a Gpu blit from it, `Flip`), so a menu frame that
locks its back buffer three times costs one upload, not one per `Unlock`. A Gpu fill or an
unkeyed Gpu blit covering the whole marked area drops it instead of uploading it. `Blt` and
`BltFast` run on the host copy when both host copies are current and share a format, and on the
Gpu otherwise. A colour fill also fills a host copy that was current, so that the 2D screens,
which mostly fill, lock and blit, never read back; a Direct3D target (a surface whose texture is
a render target) is filled on the Gpu, so the clear before a scene costs no upload. The primary
is shown by `Flip`, which copies the first back buffer to the front (the back buffer keeps its
content) and presents it: on the Gpu when the back buffer is a Direct3D target, on the host copy
when the game only draws in 2D with both host copies current. A primary without back buffers is
presented after each write. A host write made while a scene draws into a surface (between
`BeginScene` and `EndScene`) is uploaded when the scene's drawing is recorded, over the pixels
drawn so far: where a host write and a draw overlap, the host write wins.

**The scaled frame.** The game draws at its own resolution and the Gpu shows that frame in a
window of any size, centred with its aspect kept (`platform::presentRect`), the bars black. The
window can be dragged, set by `--windowed WxH`, or be the display (`--fullscreen`); none of this
reaches the module: the surfaces, their sizes and their uploads per frame are the same at every
window size, and a window of the frame's own size shows it pixel for pixel. Before its first
present the module gives the Gpu the scaling filter the runtime switch `fix_smooth_scaled_frame`
names (on, the default: sharp-bilinear, nearest at a whole-number scale; off: nearest at every
scale), and again only if the switch's position changes. Each present also gives the Window the
position of `fix_integer_scaled_frame` (off by default): on, the frame is shown at the largest
whole-number scale that fits, black bars beyond. Mouse positions come back in the game's pixels
from the Window, which maps them through the same placement.

**The render scale.** With the runtime switch `fix_render_scale` on (the default), the 3D scene is
drawn at the window's pixel density, not stretched up from the game's size. At the start of each
Direct3D scene the module works out the scale k the window asks for
(`DirectDraw::WantedRenderScale`): the frame placed in the window's drawable pixels by
`platform::framePlacement`, the chooser the present and the mouse use, and k that placement's
scale rounded up, at most what the Gpu's largest texture allows and the client's
`--render-scale N` (0, the default, follows the window; 1 keeps the game's size). If k changed,
the display (the Gpu's back buffer) is made again at k (`Gpu::ConfigureScaledBackBuffer`), and the
scene's target and its z-buffer are made again at k, the target's content read back first and
uploaded into the new texture. A window resized since the last scene changes k at the next scene.
Everything the game sees stays at its own size: surface sizes, locks, rectangles, viewports and
uploads per frame. Before the first scene, and after a mode change until the next, the display is
at 1, so frames the game draws only on the host (the menus) are shown as before; once the game
has drawn a scene they keep the scale, each upload shown enlarged by k. A lock of a scaled target
reads each k by k block back as its average, so the area it hands out is not uploaded whole: the
surface keeps a copy of it, and the next flush sends only the pixels that differ from that copy,
packed with a key in a texture of the game's size and blitted with the key
(`ChangedPixels`); a read-only lock, or one that changes nothing, sends nothing. The game locks the
back buffer each combat frame to draw its panels and text, and the rest of the scene keeps its
detail.

The game makes two DirectDraw objects. Only the one that set the display mode owns its Gpu's back
buffer (`DirectDraw::OwnsDisplay`; the owner is kept per Gpu); another object never configures it, so it cannot replace the
game's frame with a black one. Screenshots (`--shots`) read the presented frame of the owner.

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

With `SFCOP_DDRAW4_STATS=1` in the environment the module prints, once a second, the frames it
presented, the surface uploads (`WriteTexture` calls) it made, the game's frame size and the
window's drawable size in pixels:

    SFCOP_DDRAW4_STATS=1 build/sfcop --install GAME --data DATA --windowed --shots SHOTS --shot-every 5

On the main menu this reads `[ddraw4] 60.0 fps, 60.0 uploads/s, frame 1024x768, window 2048x1536`
(on a display of two pixels per point): one upload of the back buffer
per frame, which the game redraws whole each frame (before deferral, one upload per `Unlock`,
about 1,300 a second). The shots in `SHOTS` are the frames presented, and match a capture of the
window. Two such shot directories, A and B, compare with:

    python3 src/hle/ddraw4/tools/framediff.py A B
