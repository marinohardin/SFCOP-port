# src/platform/Gpu: surfaces, fixed-function drawing, present

The renderer the DirectDraw 4 and Direct3D 3 modules draw through (DESIGN §6.2). `Gpu.h` is the
contract; `src/platform/README.md` lists the backends and what each one supports.

| File | What |
| --- | --- |
| `Gpu.h` | The contract: textures, palettes, buffers, pipelines, scenes, blits, fills, the back buffer and `Present()`, `presentRect` |
| `GpuSdl.cpp` | The SDL GPU backend (`makeGpu()`) |
| `GpuSdlPixels.cpp` | Pixel conversion between the game's formats and the GPU's BGRA8 |
| `GpuSdlShaders.cpp` | The one vertex and one fragment program, in each shader format the backend takes |
| `GpuStub.cpp` | `makeGpuStub()`: answers `kUnsupported` to everything |
| `GpuMock.h` | The test double: records every call, answers what a test sets, keeps each texture's render scale |
| `GpuTest.cpp` | `presentRect`, the mock, the stub, the defaults of methods added after the freeze |
| `GpuSdlTest.cpp` | The SDL backend on a real device, rendering offscreen and reading back; skips without a GPU (`SFCOP_REQUIRE_GPU=1` makes that a failure) |

## Render scales

A texture may carry a render scale k (`TextureDesc::renderScale`, 1 by default): its image on the
GPU is k times its logical size in each direction. The scale exists so the game's Direct3D scene
can be rasterised at the window's pixel density while the game keeps drawing at the size it chose.

- Every value crossing the contract stays logical: the texture's width and height, vertex
  positions, viewports, clear rectangles, blit and fill rectangles, read and write regions. Only
  the GPU work is done at k times the size.
- A draw keeps its vertex positions in logical pixels; the vertex shader maps them with the
  logical target size, so the same triangle covers k times as many pixels each way. Direct3D's
  pixel centre is at a whole coordinate and the shader adds 0.5 logical pixel to reach the GPU's
  centre convention; at scale k that is 0.5·k physical pixels, the centre of the k by k block
  that stands for one logical pixel. Logical vertex p therefore rasterises exactly as vertex
  k·p + (k−1)/2 does on a k=1 target k times the size.
- A write (`WriteTexture`, the expansion of an indexed texture) is shown enlarged, each pixel a k
  by k block, the nearest-pixel choice the present filter makes at a whole-number scale. A fill
  writes its value at the full size.
- A read (`ReadTexture`) averages each k by k block, rounding to nearest, into one pixel. Where
  the image has no detail finer than a logical pixel the read gives back what was written.
- A blit's rectangles are logical; between textures of different scales it stretches with point
  sampling, so a blit of a whole k-scaled texture into a k=1 texture k times its size copies the
  image pixel for pixel.
- A scene's depth target has the colour target's scale.
- `ConfigureScaledBackBuffer` makes a back buffer with a scale; `Present()` samples its whole
  image into `presentRect`, which is still worked out from the logical size.
- k > 1 is refused for an indexed texture and for a texture with mipmaps, and where k times the
  size exceeds `GpuCaps::maxTextureSize`. A Gpu that does not implement scales answers
  `kUnsupported` to every scale but 1 on the back buffer.

At k = 1 every path is the one it was before scales existed.
