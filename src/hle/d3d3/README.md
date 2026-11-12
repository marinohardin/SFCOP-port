# d3d3: Direct3D 3 fixed function on Gpu

The Direct3D the game draws its 3D scenes with: `IDirect3D3`, `IDirect3DDevice3`,
`IDirect3DTexture2` and `IDirect3DViewport3` on the `Gpu` component (`src/platform/Gpu/`), drawing
into the surfaces of `../ddraw4/`. `../surface.tsv` says what each method does.

## Layers

| Files | What |
|---|---|
| `D3d3Defs.h` | Direct3D constants, render and stage states, and the structures the module uses |
| `Direct3D.*` | The device enumeration (one hardware device) and the factory for devices and viewports |
| `Device.*` | States, transforms, viewports, scenes, draws and clears |
| `Vertices.*` | Which vertex formats reach the Gpu as they are, and the repacking of the others |
| `Viewport.*` | The viewport rectangle and clip volume; clears through its device |
| `Texture.*` | A surface's texture view: the legacy texture handle (it lives as long as the surface, not the view) and `Load` |
| `Views.*` | QueryInterface for the Direct3D objects: which view an IID names, and the objects made on the way |
| `*Test.cpp`, `D3d3TestRig.h` | The doctests, through `GpuMock` on a flipping 800x600x16 display |
| `D3d3Guest.cpp` | The guest shims (compiled only with the lift) |

## Drawing

The render and texture stage states the game sets become a `PipelineDesc` at each draw
(`Device::CurrentPipeline`), cached by its content; the per-draw values (alpha reference, fog
colour, texture factor, the stage textures) go in the `DrawState`. The Direct3D 5 texture states
(`TEXTUREHANDLE`, `TEXTUREMAPBLEND`, `TEXTUREMAG`, `TEXTUREMIN`, `TEXTUREADDRESS*`) set stage 0. A
stage with no texture passes the previous stage's colour where it would sample.

Vertices are written to one stream buffer (1 MiB) and indices to another (256 KiB), each written
from where the last draw ended and started again from the beginning when full. Transformed
vertices with at most two coordinate sets and no normal go to the Gpu unchanged. Others are
repacked to that form: untransformed positions are transformed on the CPU by world, view and
projection and mapped through the current viewport, without clipping: the Gpu receives screen
positions and 1/w and promises no clipping on it, so untransformed vertices with w <= 0 are not
clipped away. Normals are dropped, nothing is lit, and missing colours are white.

`BeginScene` starts a Gpu scene on the device's target and its attached z-buffer, in the current
viewport. First it gives both the render scale DirectDraw chooses for the scene
(`DirectDraw::BeginSceneScale`, `fix_render_scale`; ddraw4's README), so a 3D scene is drawn at
the window's pixel density while the device, its viewports and its vertices stay in the game's
pixels. At a scale over 1, points and lines are drawn as triangles that cover what they cover at
the game's own size (`widenPrimitives`): a point one game pixel square, a line one game pixel
wide; the Gpu would draw them one target pixel wide, a k-th of their width. The stars of the
combat scene are points. A clear inside a scene clears on the Gpu: the `Gpu` clips a clear to its own current
viewport, so the Gpu viewport is set from the clearing `IDirect3DViewport3` (rectangle and depth
range) for the clear and put back to the device's current one afterwards; the area is in target
pixels. A clear with no rectangles clears the whole viewport. Outside a scene a clear fills the
surfaces.
`EndScene` marks the target as drawn by the Gpu only if something was drawn.

## The guest layer

Direct3D has no DLL export; `sfcop_hle_d3d3[]` is empty. The game reaches it through
QueryInterface on DirectDraw objects: `IDirect3D3` from a DirectDraw object (one Direct3D object
per DirectDraw object) and `IDirect3DTexture2` from a surface (one texture per surface).
`Views.*` holds that decision, tested without guest memory (`QueryInterfaceTest.cpp`);
`D3d3Guest.cpp` installs it as the hook the DirectDraw shims call for an interface they do not
serve (`GuestViews::SetQueryHook`, `../ddraw4/GuestViews.h`). The rest follows `../ddraw4/README.md`.

## What it does not do

- Lights, materials, `LightElements`, `TransformVertices`, vertex buffers and the Direct3D 2
  `Begin`/`Vertex`/`End` drawing: `DDERR_UNSUPPORTED`.
- States kept and read back but without effect on the drawing, because the Gpu has no state for
  them: stencil, `ZBIAS`, wrap states, anti-aliasing, dithering, `LIGHTING` and the other
  lighting states (nothing is lit), `BORDERCOLOR`, the mip LOD bias, the DX6 texture-argument
  modifiers (complement, alpha replicate), texture-coordinate generation flags, texture-transform
  flags, and the texture stage operations the pipeline description has no equivalent for.
- More than two texture stages: `ValidateDevice` says `D3DERR_TOOMANYOPERATIONS`.
- 16-bit z-buffers only.

## Tests

`sfcop_tests` (the doctests) and `sh scripts/check.sh`. The shim tables are checked against
`../surface.tsv` by `../ddraw4/tools/check_tables.py` (see `../ddraw4/README.md`). One test sets a sequence of render and
stage states and checks the one `PipelineDesc` they give, field by field.

## Proof run

Pending B1 (the SDL backends) and B6 (the guest runtime), as `../ddraw4/README.md` says.
