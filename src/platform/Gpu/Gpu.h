// Gpu: what the DirectDraw 4 and Direct3D 3 HLE modules need to draw the game (DESIGN §4.7,
// §5.6, §6.2): textures in the formats DirectDraw surfaces use (with palettes and colour keys),
// the back buffer, vertex and index buffers, pipelines carrying the fixed-function state
// Direct3D 3 sets, draws in the game's vertex layouts, 2D blits and fills, readback, present.
// It models Direct3D 3's fixed function, not a general engine. Pixel identity with the original
// is not required (§5.6); geometry is.
//
// Objects are created and used through handles. A handle belongs to the Gpu that made it and
// dies when destroyed, at Detach(), or when the back buffer it was is reconfigured.
//
// Colours are 32-bit 0xAARRGGBB (in memory, little-endian: B, G, R, A), as the game's vertices
// and clear colours hold them. Pixel data is tightly packed rows of the texture's format; the row
// pitch argument gives the bytes from one row's start to the next.
//
// Threading: every method runs on the main thread, the one that runs Window::PumpEvents(). Attach()
// and Present() must, because the swapchain is acquired on the window's thread; the other
// methods are held to the same thread so that a backend needs no locking. Calls from another
// thread are a programming error.
#pragma once

#include "platform/Core/Handle.h"
#include "platform/Core/Result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace sfcop::platform {

class Window;

struct BufferTag {};
using BufferId = Handle<BufferTag>;
struct TextureTag {};
using TextureId = Handle<TextureTag>;
struct PaletteTag {};
using PaletteId = Handle<PaletteTag>;
struct PipelineTag {};
using PipelineId = Handle<PipelineTag>;

enum class PixelFormat : std::uint8_t {
    kRgb565,   // 16-bit: 5 red, 6 green, 5 blue
    kArgb1555, // 16-bit: 1 alpha, 5 red, 5 green, 5 blue
    kXrgb8888, // 32-bit: 8 unused, 8 red, 8 green, 8 blue
    kArgb8888, // 32-bit: 8 alpha, 8 red, 8 green, 8 blue
    kIndexed8, // 8-bit index into the attached palette
    kDepth16,  // 16-bit depth; only as a depth target
};

// Bytes per pixel of a format.
constexpr std::uint32_t pixelBytes(PixelFormat format) {
    switch (format) {
    case PixelFormat::kRgb565:
    case PixelFormat::kArgb1555:
    case PixelFormat::kDepth16:
        return 2;
    case PixelFormat::kXrgb8888:
    case PixelFormat::kArgb8888:
        return 4;
    case PixelFormat::kIndexed8:
        return 1;
    }
    return 0;
}

// A rectangle in pixels. A width or height of 0 means "to the edge" from x, y; so Rect{} is the
// whole surface.
struct Rect {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct TextureDesc {
    std::uint32_t width = 0;  // 1 to GpuCaps::maxTextureSize
    std::uint32_t height = 0;
    PixelFormat format = PixelFormat::kRgb565;
    std::uint8_t mipLevels = 1; // 1 = no mipmaps; level n is (width >> n) by (height >> n)
    bool renderTarget = false;  // may be a colour or depth target of BeginScene()
    // The render scale k: the texture's image is k times its logical size in each dimension
    // (width * k by height * k pixels, each at most GpuCaps::maxTextureSize), so that the game's
    // geometry rasterises at a finer pixel density while the game sees its own size. Everything a
    // caller passes or reads stays in logical pixels: rectangles, viewports, clears, blits,
    // vertex positions, and the pixels of WriteTexture() and ReadTexture(). A write is shown
    // enlarged by k (each logical pixel a k by k block); a read returns each k by k block's
    // average; a draw covers the same logical area as at k = 1, with k times the detail. 1 is
    // the texture as it always was. k > 1 requires mipLevels 1 and a format other than kIndexed8.
    std::uint8_t renderScale = 1;
};

struct PaletteEntry {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    std::uint8_t alpha = 255;
};
constexpr std::size_t kPaletteSize = 256;

enum class BufferKind : std::uint8_t {
    kVertex,
    kIndex, // 16-bit unsigned indices
};

// A vertex layout, in the order of its fields in memory: position (3 floats; with kXyzRhw a 4th,
// the reciprocal of w, and the position is already in screen pixels and depth 0-1), normal
// (3 floats), diffuse colour (32-bit), specular colour (32-bit; its alpha is the vertex fog
// factor), then `texCoordSets` pairs of floats u, v. The game draws transformed and lit vertices:
// kXyzRhw, diffuse, specular, one set (32 bytes).
enum class PositionKind : std::uint8_t { kXyz, kXyzRhw };

struct VertexLayout {
    PositionKind position = PositionKind::kXyzRhw;
    bool normal = false;
    bool diffuse = true;
    bool specular = true;
    std::uint8_t texCoordSets = 1; // 0 to 2
};

// Bytes from one vertex to the next in a layout.
constexpr std::uint32_t vertexStride(const VertexLayout &layout) {
    return (layout.position == PositionKind::kXyzRhw ? 16u : 12u) + (layout.normal ? 12u : 0u) +
           (layout.diffuse ? 4u : 0u) + (layout.specular ? 4u : 0u) + 8u * layout.texCoordSets;
}

enum class PrimitiveType : std::uint8_t {
    kPointList,
    kLineList,
    kLineStrip,
    kTriangleList,
    kTriangleStrip,
    kTriangleFan, // the backend converts to lists where the API has no fans
};

enum class BlendFactor : std::uint8_t {
    kZero,
    kOne,
    kSourceColor,
    kInverseSourceColor,
    kSourceAlpha,
    kInverseSourceAlpha,
    kDestinationAlpha,
    kInverseDestinationAlpha,
    kDestinationColor,
    kInverseDestinationColor,
    kSourceAlphaSaturate,
};

enum class CompareFunction : std::uint8_t {
    kNever,
    kLess,
    kEqual,
    kLessEqual,
    kGreater,
    kNotEqual,
    kGreaterEqual,
    kAlways,
};

enum class CullMode : std::uint8_t {
    kNone,
    kClockwise,        // triangles wound clockwise on screen are discarded
    kCounterClockwise, // the Direct3D default
};

enum class FillMode : std::uint8_t { kPoint, kWireframe, kSolid };
enum class ShadeMode : std::uint8_t { kFlat, kGouraud };

enum class FogMode : std::uint8_t {
    kNone,
    kVertex, // the factor is the specular colour's alpha (255 = no fog)
    kLinear, // per pixel from depth, between DrawState::fogStart and fogEnd
    kExp,    // per pixel, exp(-density * depth)
    kExp2,   // per pixel, exp(-(density * depth)^2)
};

enum class TextureOp : std::uint8_t {
    kDisable, // this stage and every later one are off
    kSelectArg1,
    kSelectArg2,
    kModulate,
    kModulate2x,
    kModulate4x,
    kAdd,
    kAddSigned,
    kSubtract,
    kBlendDiffuseAlpha,
    kBlendTextureAlpha,
    kBlendFactorAlpha,
    kBlendCurrentAlpha,
};

enum class TextureArg : std::uint8_t {
    kCurrent, // the previous stage's result (the diffuse colour in stage 0)
    kDiffuse,
    kSpecular,
    kTexture, // this stage's texture
    kTextureFactor,
};

enum class TextureAddress : std::uint8_t { kWrap, kMirror, kClamp };
enum class TextureFilter : std::uint8_t { kPoint, kLinear };
enum class MipFilter : std::uint8_t { kNone, kPoint, kLinear };

// One texture stage's fixed-function setup. The defaults are Direct3D's for stage 0.
struct TextureStage {
    TextureOp colorOp = TextureOp::kModulate;
    TextureArg colorArg1 = TextureArg::kTexture;
    TextureArg colorArg2 = TextureArg::kCurrent;
    TextureOp alphaOp = TextureOp::kSelectArg1;
    TextureArg alphaArg1 = TextureArg::kTexture;
    TextureArg alphaArg2 = TextureArg::kCurrent;
    TextureAddress addressU = TextureAddress::kWrap;
    TextureAddress addressV = TextureAddress::kWrap;
    TextureFilter magFilter = TextureFilter::kPoint;
    TextureFilter minFilter = TextureFilter::kPoint;
    MipFilter mipFilter = MipFilter::kNone;
    std::uint8_t texCoordIndex = 0; // which of the vertex's coordinate sets to sample with
};

constexpr std::size_t kTextureStages = 2;

// Everything a pipeline fixes at creation. Values that change between draws without a new
// pipeline are in DrawState.
struct PipelineDesc {
    VertexLayout layout;
    PrimitiveType primitive = PrimitiveType::kTriangleList;
    PixelFormat colorFormat = PixelFormat::kRgb565; // the colour target's format
    bool depthTarget = false;                        // a kDepth16 target will be bound
    bool blend = false;
    BlendFactor sourceBlend = BlendFactor::kOne;
    BlendFactor destinationBlend = BlendFactor::kZero;
    bool depthTest = false;
    bool depthWrite = true;
    CompareFunction depthFunction = CompareFunction::kLessEqual;
    bool alphaTest = false;
    CompareFunction alphaFunction = CompareFunction::kAlways; // against DrawState::alphaReference
    CullMode cull = CullMode::kCounterClockwise;
    FillMode fill = FillMode::kSolid;
    ShadeMode shade = ShadeMode::kGouraud;
    bool specular = false; // add the specular colour after texturing
    bool colorKey = false; // discard texels equal to their texture's colour key
    FogMode fog = FogMode::kNone;
    std::array<TextureStage, kTextureStages> stages{
        TextureStage{},
        TextureStage{TextureOp::kDisable, TextureArg::kTexture, TextureArg::kCurrent,
                     TextureOp::kDisable, TextureArg::kTexture, TextureArg::kCurrent,
                     TextureAddress::kWrap, TextureAddress::kWrap, TextureFilter::kPoint,
                     TextureFilter::kPoint, MipFilter::kNone, 1},
    };
};

// The per-draw values Direct3D 3 sets as render states.
struct DrawState {
    std::array<TextureId, kTextureStages> textures{}; // zero = no texture in that stage
    std::uint32_t fogColor = 0;                       // 0x00RRGGBB
    float fogStart = 0.0f;
    float fogEnd = 1.0f;
    float fogDensity = 1.0f;
    std::uint8_t alphaReference = 0;
    std::uint32_t textureFactor = 0xFFFFFFFF; // TextureArg::kTextureFactor
};

struct DrawCall {
    PipelineId pipeline;
    BufferId vertices;
    std::uint32_t firstVertex = 0;
    std::uint32_t vertexCount = 0;
    BufferId indices; // zero = not indexed
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
    DrawState state;
};

struct Viewport {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0; // 0 = to the target's edge
    std::uint32_t height = 0;
    float minDepth = 0.0f;
    float maxDepth = 1.0f;
};

struct ClearDesc {
    bool color = true;
    bool depth = false;
    std::uint32_t colorValue = 0xFF000000; // 0xAARRGGBB
    float depthValue = 1.0f;
    // In target pixels, clipped to the current viewport; Rect{} = the whole viewport.
    Rect area;
};

struct RenderTargets {
    TextureId color; // a renderTarget texture or the back buffer
    TextureId depth; // a kDepth16 renderTarget texture of the same size, or zero
};

struct BlitDesc {
    TextureId source;
    Rect sourceRect;
    TextureId destination;
    Rect destinationRect; // a different size stretches, with point sampling
    bool sourceColorKey = false; // skip source pixels equal to the source's colour key
};

// Where Present() puts a frame of frameWidth by frameHeight pixels in a target of targetWidth by
// targetHeight pixels: the largest rectangle of the frame's aspect that fits, centred, its size
// rounded to the nearest pixel; the rest of the target is black bars. A frame the target's size
// fills it exactly. A zero size anywhere gives a rectangle of width and height 0 at 0, 0.
constexpr Rect presentRect(std::uint32_t frameWidth, std::uint32_t frameHeight,
                           std::uint32_t targetWidth, std::uint32_t targetHeight) {
    if (frameWidth == 0 || frameHeight == 0 || targetWidth == 0 || targetHeight == 0) {
        return Rect{};
    }
    const std::uint64_t fw = frameWidth;
    const std::uint64_t fh = frameHeight;
    const std::uint64_t tw = targetWidth;
    const std::uint64_t th = targetHeight;
    std::uint64_t width = tw;
    std::uint64_t height = th;
    if (tw * fh <= th * fw) { // the target is narrower than the frame: bars above and below
        height = (fh * tw + fw / 2) / fw;
    } else { // wider: bars left and right
        width = (fw * th + fh / 2) / fh;
    }
    Rect out;
    out.x = static_cast<std::int32_t>((tw - width) / 2);
    out.y = static_cast<std::int32_t>((th - height) / 2);
    out.width = static_cast<std::uint32_t>(width);
    out.height = static_cast<std::uint32_t>(height);
    return out;
}

struct GpuCaps {
    std::uint32_t maxTextureSize = 0; // in each dimension
    std::vector<PixelFormat> textureFormats;
    bool linearFiltering = false;
};

class Gpu {
public:
    virtual ~Gpu() = default;

    // Binds the Gpu to a window. Main thread only. The window must be open and must come from a
    // backend this Gpu can use. Errors: kUnsupported; kWrongState
    // if already attached; kInvalidArgument for a closed or foreign window; kIoError if no
    // device is available.
    virtual Status Attach(Window &window) = 0;

    // Waits for submitted work, destroys every object, and unbinds the window. Detaching a
    // detached Gpu succeeds. Errors: kUnsupported.
    virtual Status Detach() = 0;

    // What the device supports. Errors: kUnsupported; kWrongState if not attached.
    virtual Result<GpuCaps> GetCaps() = 0;

    // (Re)creates the back buffer at the game's resolution and format, cleared to black, and
    // returns its texture. A previous back buffer handle dies. Present() scales it to the window
    // keeping its aspect. Errors: kUnsupported; kWrongState if not attached or inside a scene;
    // kInvalidArgument for a zero size, a size over maxTextureSize, or kDepth16;
    // kOutOfResources.
    virtual Result<TextureId> ConfigureBackBuffer(std::uint32_t width, std::uint32_t height,
                                                  PixelFormat format) = 0;

    // Creates a buffer of `bytes`, content undefined. Errors: kUnsupported; kWrongState if not
    // attached; kInvalidArgument for 0 bytes or an index buffer of odd size; kOutOfResources.
    virtual Result<BufferId> CreateBuffer(BufferKind kind, std::uint32_t bytes) = 0;

    // Copies `bytes` into a buffer at `offset`. Draws already recorded keep the data they saw.
    // Errors: kUnsupported; kInvalidHandle; kInvalidArgument for a range past the end.
    virtual Status WriteBuffer(BufferId buffer, std::uint32_t offset, const void *data,
                               std::uint32_t bytes) = 0;

    // Frees a buffer. Errors: kUnsupported; kInvalidHandle.
    virtual Status DestroyBuffer(BufferId buffer) = 0;

    // Creates a texture, content undefined. Errors: kUnsupported; kWrongState if not attached;
    // kInvalidArgument for a zero or oversize dimension, more mip levels than the size allows, a
    // format the caps do not list, or kDepth16 without renderTarget; kOutOfResources.
    virtual Result<TextureId> CreateTexture(const TextureDesc &desc) = 0;

    // Replaces a region of one mip level with `pixels` in the texture's format, `rowPitch` bytes
    // apart. Errors: kUnsupported; kInvalidHandle; kInvalidArgument for a level or region outside
    // the texture, a pitch under width * pixelBytes, or a kDepth16 texture; kWrongState if the
    // texture is the colour target of the current scene.
    virtual Status WriteTexture(TextureId texture, std::uint8_t level, Rect region,
                                const void *pixels, std::uint32_t rowPitch) = 0;

    // Copies a region of one mip level into `out`, `rowPitch` bytes apart, in the texture's
    // format; waits for the rendering that writes it. For tests and for the game's reads of a
    // locked surface. Errors: as WriteTexture().
    virtual Status ReadTexture(TextureId texture, std::uint8_t level, Rect region, void *out,
                               std::uint32_t rowPitch) = 0;

    // Frees a texture. The back buffer cannot be destroyed this way. Errors: kUnsupported;
    // kInvalidHandle; kWrongState if it is a target of the current scene or the back buffer.
    virtual Status DestroyTexture(TextureId texture) = 0;

    // Sets or clears (std::nullopt) a texture's colour key: a raw pixel value in its format.
    // Used by pipelines with colorKey and by blits with sourceColorKey. Errors: kUnsupported;
    // kInvalidHandle; kInvalidArgument for a value with bits outside the format, or kDepth16.
    virtual Status SetColorKey(TextureId texture, std::optional<std::uint32_t> key) = 0;

    // Creates a palette of kPaletteSize entries copied from `entries`. Errors: kUnsupported;
    // kWrongState if not attached; kOutOfResources.
    virtual Result<PaletteId> CreatePalette(const PaletteEntry *entries) = 0;

    // Replaces `count` entries from `first`; textures using the palette show the change in draws
    // and blits from now on. Errors: kUnsupported; kInvalidHandle; kInvalidArgument for a range
    // past 256.
    virtual Status WritePalette(PaletteId palette, std::uint32_t first, std::uint32_t count,
                                const PaletteEntry *entries) = 0;

    // Frees a palette; textures it was attached to have none. Errors: kUnsupported;
    // kInvalidHandle.
    virtual Status DestroyPalette(PaletteId palette) = 0;

    // Attaches a palette to a kIndexed8 texture (zero detaches). An indexed texture without a
    // palette samples as black. Errors: kUnsupported; kInvalidHandle; kInvalidArgument if the
    // texture is not kIndexed8.
    virtual Status AttachPalette(TextureId texture, PaletteId palette) = 0;

    // Builds a pipeline. Creating one is slow; the HLE module keeps them by description.
    // Errors: kUnsupported; kWrongState if not attached; kInvalidArgument for an inconsistent
    // description (kIndexed8 or kDepth16 colour format, more coordinate sets than the layout
    // has, a stage enabled after a disabled one); kOutOfResources.
    virtual Result<PipelineId> CreatePipeline(const PipelineDesc &desc) = 0;

    // Frees a pipeline. Errors: kUnsupported; kInvalidHandle.
    virtual Status DestroyPipeline(PipelineId pipeline) = 0;

    // Starts drawing into `targets`; the viewport is reset to the whole colour target. Errors:
    // kUnsupported; kWrongState if not attached or already in a scene; kInvalidHandle;
    // kInvalidArgument for a texture that is not a render target, a depth target of another size
    // or render scale, or kDepth16 as colour.
    virtual Status BeginScene(const RenderTargets &targets) = 0;

    // Sets the viewport for later clears and draws in this scene. Errors: kUnsupported;
    // kWrongState outside a scene; kInvalidArgument for an area outside the target or depths out
    // of 0-1 or out of order.
    virtual Status SetViewport(const Viewport &viewport) = 0;

    // Clears colour and/or depth in an area given in target pixels and clipped to the current
    // viewport (ClearDesc::area). Errors: kUnsupported; kWrongState outside a scene, or depth
    // without a depth target.
    virtual Status Clear(const ClearDesc &clear) = 0;

    // Draws primitives from a buffer, indexed or not. Errors: kUnsupported; kWrongState outside
    // a scene, or if the pipeline's colour format or depth target do not match the scene's;
    // kInvalidHandle; kInvalidArgument for a vertex or index range past its buffer, a count that
    // does not make whole primitives, or a texture in a stage the pipeline has disabled.
    virtual Status Draw(const DrawCall &draw) = 0;

    // Ends the scene; its drawing is submitted. Errors: kUnsupported; kWrongState outside a
    // scene.
    virtual Status EndScene() = 0;

    // Copies a rectangle from one texture to another (the same texture only if the rectangles
    // do not overlap). The rectangles are logical pixels of each texture; between textures of
    // different render scales the copy stretches as between rectangles of different sizes. The
    // formats must match, except that kIndexed8 converts through the source's palette into a
    // direct-colour destination. Outside a scene only. Errors:
    // kUnsupported; kWrongState inside a scene; kInvalidHandle; kInvalidArgument for rectangles
    // outside their textures, a format mismatch, or kDepth16.
    virtual Status Blit(const BlitDesc &blit) = 0;

    // Sets every pixel of an area to a raw value in the texture's format (a depth value for
    // kDepth16). Outside a scene only. Errors: kUnsupported; kWrongState inside a scene;
    // kInvalidHandle; kInvalidArgument for an area outside the texture.
    virtual Status Fill(TextureId texture, Rect area, std::uint32_t value) = 0;

    // As ConfigureBackBuffer(), with a render scale (TextureDesc::renderScale): the back buffer's
    // image is renderScale times its logical size, and Present() scales that image into the
    // window. A scale of 1 is ConfigureBackBuffer(). Errors: as ConfigureBackBuffer(), and
    // kInvalidArgument for a scale of 0, a scaled size over maxTextureSize, or kIndexed8 with a
    // scale over 1. A Gpu that does not implement render scales answers kUnsupported for every
    // scale but 1.
    virtual Result<TextureId> ConfigureScaledBackBuffer(std::uint32_t width, std::uint32_t height,
                                                        PixelFormat format,
                                                        std::uint8_t renderScale) {
        if (renderScale == 1) {
            return ConfigureBackBuffer(width, height, format);
        }
        return Error::kUnsupported;
    }

    // Shows the back buffer in the window at presentRect() of the window's size in pixels, scaled
    // with the filter SetPresentFilter() set, with its aspect kept, and waits as the
    // window's frame cap requires; a back buffer with a render scale is sampled from its full
    // image, placed by its logical size. Main thread only. The back buffer's content is kept.
    // Errors:
    // kUnsupported; kWrongState if not attached, inside a scene, or no back buffer is configured;
    // kIoError if the device was lost (the caller reconfigures and redraws).
    virtual Status Present() = 0;

    // How Present() samples the back buffer when it scales it into the window (presentRect()).
    // kPoint: nearest at every scale. kLinear: linear, except nearest at a whole-number scale; this
    // is the default. A scale of 1 shows the back buffer's pixels as they are either way. Applies
    // from the next Present(). Main thread only. Errors: kUnsupported (also the answer of a Gpu
    // that does not implement it); kInvalidArgument for a value that is not a TextureFilter.
    virtual Status SetPresentFilter(TextureFilter filter) {
        static_cast<void>(filter);
        return Error::kUnsupported;
    }
};

// A Gpu whose every method fails with kUnsupported.
std::unique_ptr<Gpu> makeGpuStub();

// The build's real backend, on SDL GPU (B1); declared here so that the client needs no
// backend header, defined by the backend's own source file once it lands.
std::unique_ptr<Gpu> makeGpu();

} // namespace sfcop::platform
