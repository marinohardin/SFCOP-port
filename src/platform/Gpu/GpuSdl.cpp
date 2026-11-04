// The Gpu backend on SDL GPU (DESIGN §5.6, §6.2): Direct3D 3's fixed function on one pair of
// shader programs (GpuSdlShaders.cpp).
//
// How it is built:
// - Every colour texture is an 8-bit B, G, R, A GPU texture usable as sampler and colour target;
//   the game's formats are converted on the way in and out (GpuSdlPixels.cpp). An indexed texture
//   also keeps its indices in memory and is re-expanded through its palette when either changes.
//   A depth texture is a 16-bit GPU depth target.
// - Buffers live in memory. Draw() copies the vertices and indices it uses into the scene's
//   staging data, so a later WriteBuffer() cannot change a recorded draw; the staging data is
//   uploaded in one copy pass when the scene is encoded. Triangle fans become lists there.
// - A scene records its clears and draws and encodes them as one render pass at EndScene(). A
//   texture or palette write during the scene encodes what was recorded first, so earlier draws
//   keep the content they saw.
// - Clears inside a scene, blits and depth fills are draws of a screen quad through the same
//   programs, with internal pipelines.
// - Present() blits the back buffer into the swapchain, letterboxed, then waits for the frame
//   cap: sleep, then spin to the deadline. For a window created hidden (SFCOP_WINDOW_HIDDEN=1)
//   or on SDL's dummy and offscreen video drivers there is no swapchain and Present() only
//   paces, which is what headless runs and tests use. Metal needs a video driver that makes
//   Metal views, so on macOS a device exists only with the Cocoa driver, hidden window or not.
#include "platform/Gpu/Gpu.h"
#include "platform/Window/Window.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sfcop::platform {

// Defined in GpuSdlShaders.cpp and GpuSdlPixels.cpp. Declared here rather than in a header,
// because every header under src/platform/ is a contract.
namespace gpusdl {
std::uint32_t shaderFormats();
bool shaderCode(std::uint32_t format, bool fragment, const std::uint8_t *&code, std::size_t &size,
                const char *&entry);
std::uint32_t formatMask(PixelFormat format);
std::uint32_t rawToArgb(PixelFormat format, std::uint32_t raw, const PaletteEntry *palette);
std::uint32_t argbToRaw(PixelFormat format, std::uint32_t argb);
void toBgra(PixelFormat format, const std::uint8_t *src, std::uint8_t *dst, std::uint32_t count,
            const PaletteEntry *palette);
void fromBgra(PixelFormat format, const std::uint8_t *src, std::uint8_t *dst, std::uint32_t count);

// What a failure to create a device means, given what the driver named can take. A driver that
// accepts none of the shader formats this build carries, but one it does not, is a build that
// lacks a format (kUnsupported); any other failure is the device or the driver (kIoError).
Error noDeviceError(bool driverTakesCarriedFormat, bool driverTakesOtherFormat) {
    return !driverTakesCarriedFormat && driverTakesOtherFormat ? Error::kUnsupported
                                                               : Error::kIoError;
}

// Whether Present() must wait for the frame cap itself. Vsync paces a claimed window at the
// display's rate, but only while a swapchain image is acquired: a minimised or occluded window
// gets none and returns at once.
bool presentNeedsPacing(bool windowClaimed, bool explicitCap, bool swapchainAcquired) {
    return !windowClaimed || explicitCap || !swapchainAcquired;
}
} // namespace gpusdl

namespace {

using namespace gpusdl;

constexpr std::uint32_t kMaxTextureSize = 8192;
constexpr std::uint32_t kMaxBufferBytes = 64u * 1024u * 1024u;
constexpr std::uint32_t kDefaultRefreshHz = 60;
constexpr const char *kWindowProperty = "sfcop.platform.window"; // set by WindowSdl.cpp
constexpr SDL_GPUTextureFormat kColorFormat = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
constexpr SDL_GPUTextureFormat kDepthFormat = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
constexpr std::uint32_t kAllShaderFormats =
    SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXBC | SDL_GPU_SHADERFORMAT_DXIL |
    SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_METALLIB;

// Mirrors VertexUniforms and FragmentUniforms in the shader source, member for member.
struct VertexUniforms {
    float target[4];
    std::int32_t select[4];
};
struct FragmentUniforms {
    std::int32_t stage0a[4];
    std::int32_t stage0b[4];
    std::int32_t stage1a[4];
    std::int32_t stage1b[4];
    float key0[4];
    float key1[4];
    float textureFactor[4];
    float fogColor[4];
    float fog[4];
    std::int32_t misc[4];
};

// The transformed, lit vertex internal quads are drawn with (the default VertexLayout).
struct QuadVertex {
    float x, y, z, rhw;
    std::uint32_t diffuse;
    std::uint32_t specular;
    float u, v;
};

// The constant values a layout's missing attributes read: white diffuse, black specular with
// no fog, texture coordinates 0, 0.
struct DefaultAttributes {
    std::uint32_t diffuse = 0xFFFFFFFFu;
    std::uint32_t specular = 0xFF000000u;
    float u = 0.0f;
    float v = 0.0f;
};

struct BufferRec {
    BufferKind kind = BufferKind::kVertex;
    std::vector<std::uint8_t> bytes;
};

struct TextureRec {
    TextureDesc desc;
    SDL_GPUTexture *gpu = nullptr;
    std::optional<std::uint32_t> key;
    PaletteId palette;
    std::vector<std::vector<std::uint8_t>> indices; // kIndexed8: one array per mip level
};

struct PaletteRec {
    std::array<PaletteEntry, kPaletteSize> entries{};
};

struct PipelineRec {
    PipelineDesc desc;
    SDL_GPUGraphicsPipeline *gpu = nullptr;
    std::string key;
    std::uint32_t references = 0;
};

// One recorded draw with everything resolved to GPU objects.
struct Command {
    SDL_GPUGraphicsPipeline *pipeline = nullptr;
    SDL_Rect scissor{};
    float minDepth = 0.0f; // the viewport's depth range when the draw was recorded
    float maxDepth = 1.0f;
    std::uint32_t vertexOffset = 0;
    std::uint32_t vertexCount = 0;
    bool indexed = false;
    std::uint32_t indexOffset = 0;
    std::uint32_t indexCount = 0;
    std::array<SDL_GPUTexture *, kTextureStages> textures{};
    std::array<SDL_GPUSampler *, kTextureStages> samplers{};
    VertexUniforms vertexUniforms{};
    FragmentUniforms fragmentUniforms{};
};

// The work of one render pass: its targets, staging data and draws.
struct Pass {
    SDL_GPUTexture *color = nullptr;
    SDL_GPUTexture *depth = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> staging;
    std::vector<Command> commands;
};

std::uint32_t levelSize(std::uint32_t size, std::uint8_t level) {
    return (std::max)(1u, size >> level);
}

// Rect with "0 = to the edge" resolved against a surface; false if it is empty or outside.
bool resolveRect(const Rect &in, std::uint32_t width, std::uint32_t height, Rect &out) {
    if (in.x < 0 || in.y < 0) {
        return false;
    }
    const auto x = static_cast<std::uint32_t>(in.x);
    const auto y = static_cast<std::uint32_t>(in.y);
    if (x >= width || y >= height) {
        return false;
    }
    out.x = in.x;
    out.y = in.y;
    out.width = in.width != 0 ? in.width : width - x;
    out.height = in.height != 0 ? in.height : height - y;
    return std::uint64_t{x} + out.width <= width && std::uint64_t{y} + out.height <= height;
}

bool isColorFormat(PixelFormat format) {
    return format != PixelFormat::kDepth16 && format != PixelFormat::kIndexed8;
}

bool wholePrimitives(PrimitiveType type, std::uint32_t count) {
    switch (type) {
    case PrimitiveType::kPointList:
        return count >= 1;
    case PrimitiveType::kLineList:
        return count >= 2 && count % 2 == 0;
    case PrimitiveType::kLineStrip:
        return count >= 2;
    case PrimitiveType::kTriangleList:
        return count >= 3 && count % 3 == 0;
    case PrimitiveType::kTriangleStrip:
    case PrimitiveType::kTriangleFan:
        return count >= 3;
    }
    return false;
}

SDL_GPUBlendFactor toSdl(BlendFactor factor) {
    switch (factor) {
    case BlendFactor::kZero:
        return SDL_GPU_BLENDFACTOR_ZERO;
    case BlendFactor::kOne:
        return SDL_GPU_BLENDFACTOR_ONE;
    case BlendFactor::kSourceColor:
        return SDL_GPU_BLENDFACTOR_SRC_COLOR;
    case BlendFactor::kInverseSourceColor:
        return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
    case BlendFactor::kSourceAlpha:
        return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    case BlendFactor::kInverseSourceAlpha:
        return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::kDestinationAlpha:
        return SDL_GPU_BLENDFACTOR_DST_ALPHA;
    case BlendFactor::kInverseDestinationAlpha:
        return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
    case BlendFactor::kDestinationColor:
        return SDL_GPU_BLENDFACTOR_DST_COLOR;
    case BlendFactor::kInverseDestinationColor:
        return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
    case BlendFactor::kSourceAlphaSaturate:
        return SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE;
    }
    return SDL_GPU_BLENDFACTOR_ONE;
}

SDL_GPUCompareOp toSdl(CompareFunction function) {
    switch (function) {
    case CompareFunction::kNever:
        return SDL_GPU_COMPAREOP_NEVER;
    case CompareFunction::kLess:
        return SDL_GPU_COMPAREOP_LESS;
    case CompareFunction::kEqual:
        return SDL_GPU_COMPAREOP_EQUAL;
    case CompareFunction::kLessEqual:
        return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    case CompareFunction::kGreater:
        return SDL_GPU_COMPAREOP_GREATER;
    case CompareFunction::kNotEqual:
        return SDL_GPU_COMPAREOP_NOT_EQUAL;
    case CompareFunction::kGreaterEqual:
        return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    case CompareFunction::kAlways:
        return SDL_GPU_COMPAREOP_ALWAYS;
    }
    return SDL_GPU_COMPAREOP_ALWAYS;
}

SDL_GPUPrimitiveType toSdl(PrimitiveType type) {
    switch (type) {
    case PrimitiveType::kPointList:
        return SDL_GPU_PRIMITIVETYPE_POINTLIST;
    case PrimitiveType::kLineList:
        return SDL_GPU_PRIMITIVETYPE_LINELIST;
    case PrimitiveType::kLineStrip:
        return SDL_GPU_PRIMITIVETYPE_LINESTRIP;
    case PrimitiveType::kTriangleStrip:
        return SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
    case PrimitiveType::kTriangleList:
    case PrimitiveType::kTriangleFan: // drawn as a list (Draw converts)
        return SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    }
    return SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
}

SDL_GPUSamplerAddressMode toSdl(TextureAddress address) {
    switch (address) {
    case TextureAddress::kWrap:
        return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    case TextureAddress::kMirror:
        return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
    case TextureAddress::kClamp:
        return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    }
    return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
}

SDL_GPUFilter toSdl(TextureFilter filter) {
    return filter == TextureFilter::kLinear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
}

void argbToFloats(std::uint32_t argb, float out[4]) {
    out[0] = static_cast<float>((argb >> 16) & 0xFF) / 255.0f;
    out[1] = static_cast<float>((argb >> 8) & 0xFF) / 255.0f;
    out[2] = static_cast<float>(argb & 0xFF) / 255.0f;
    out[3] = static_cast<float>(argb >> 24) / 255.0f;
}

// Every field of a pipeline description, in order, as bytes: the pipeline cache's key.
std::string pipelineKey(const PipelineDesc &d, bool colorTarget, bool colorWrite) {
    std::string key;
    const auto put = [&key](auto value) { key.push_back(static_cast<char>(value)); };
    put(d.layout.position);
    put(d.layout.normal);
    put(d.layout.diffuse);
    put(d.layout.specular);
    put(d.layout.texCoordSets);
    put(d.primitive);
    put(d.colorFormat);
    put(d.depthTarget);
    put(d.blend);
    put(d.sourceBlend);
    put(d.destinationBlend);
    put(d.depthTest);
    put(d.depthWrite);
    put(d.depthFunction);
    put(d.alphaTest);
    put(d.alphaFunction);
    put(d.cull);
    put(d.fill);
    put(d.shade);
    put(d.specular);
    put(d.colorKey);
    put(d.fog);
    for (const TextureStage &s : d.stages) {
        put(s.colorOp);
        put(s.colorArg1);
        put(s.colorArg2);
        put(s.alphaOp);
        put(s.alphaArg1);
        put(s.alphaArg2);
        put(s.addressU);
        put(s.addressV);
        put(s.magFilter);
        put(s.minFilter);
        put(s.mipFilter);
        put(s.texCoordIndex);
    }
    put(colorTarget);
    put(colorWrite);
    return key;
}

std::uint32_t samplerKey(const TextureStage &s) {
    return static_cast<std::uint32_t>(s.addressU) | (static_cast<std::uint32_t>(s.addressV) << 4) |
           (static_cast<std::uint32_t>(s.magFilter) << 8) |
           (static_cast<std::uint32_t>(s.minFilter) << 12) |
           (static_cast<std::uint32_t>(s.mipFilter) << 16);
}

// A pipeline for internal quads: one stage that selects `colorArg`, no blending.
PipelineDesc quadPipeline(TextureArg colorArg) {
    PipelineDesc desc;
    desc.primitive = PrimitiveType::kTriangleStrip;
    desc.cull = CullMode::kNone;
    desc.depthWrite = false;
    desc.stages[0].colorOp = TextureOp::kSelectArg1;
    desc.stages[0].colorArg1 = colorArg;
    desc.stages[0].alphaOp = TextureOp::kSelectArg1;
    desc.stages[0].alphaArg1 = colorArg;
    desc.stages[0].addressU = TextureAddress::kClamp;
    desc.stages[0].addressV = TextureAddress::kClamp;
    return desc;
}

class GpuSdl final : public Gpu {
public:
    ~GpuSdl() override { (void)GpuSdl::Detach(); }

    Status Attach(Window &window) override {
        if (device_ != nullptr) {
            return Error::kWrongState;
        }
        SDL_Window *sdlWindow = FindSdlWindow(window);
        if (sdlWindow == nullptr) {
            return Error::kInvalidArgument;
        }
        const std::uint32_t formats = shaderFormats();
        SDL_GPUDevice *device = SDL_CreateGPUDevice(formats, false, nullptr);
        if (device == nullptr) {
            return NoDevice();
        }
        device_ = device;
        window_ = &window;
        sdlWindow_ = sdlWindow;
        const char *video = SDL_GetCurrentVideoDriver();
        const bool headless = video == nullptr || std::strcmp(video, "dummy") == 0 ||
                              std::strcmp(video, "offscreen") == 0 ||
                              (SDL_GetWindowFlags(sdlWindow_) & SDL_WINDOW_HIDDEN) != 0;
        if (!headless) {
            if (!SDL_ClaimWindowForGPUDevice(device_, sdlWindow_)) {
                ReleaseAll();
                return Error::kIoError;
            }
            claimed_ = true;
        }
        if (!CreateFixedObjects()) {
            ReleaseAll();
            return Error::kIoError;
        }
        return {};
    }

    Status Detach() override {
        ReleaseAll();
        return {};
    }

    Result<GpuCaps> GetCaps() override {
        if (device_ == nullptr) {
            return Error::kWrongState;
        }
        GpuCaps caps;
        caps.maxTextureSize = kMaxTextureSize;
        caps.textureFormats = {PixelFormat::kRgb565,    PixelFormat::kArgb1555,
                               PixelFormat::kXrgb8888,  PixelFormat::kArgb8888,
                               PixelFormat::kIndexed8,  PixelFormat::kDepth16};
        caps.linearFiltering = true;
        return caps;
    }

    Result<TextureId> ConfigureBackBuffer(std::uint32_t width, std::uint32_t height,
                                          PixelFormat format) override {
        if (device_ == nullptr || inScene_) {
            return Error::kWrongState;
        }
        if (width == 0 || height == 0 || width > kMaxTextureSize || height > kMaxTextureSize ||
            format == PixelFormat::kDepth16) {
            return Error::kInvalidArgument;
        }
        if (backBuffer_.Valid()) {
            ReleaseTexture(backBuffer_);
            backBuffer_ = TextureId{};
        }
        TextureDesc desc;
        desc.width = width;
        desc.height = height;
        desc.format = format;
        desc.renderTarget = format != PixelFormat::kIndexed8;
        auto made = MakeTexture(desc);
        if (!made.Ok()) {
            return made.Code();
        }
        backBuffer_ = made.Value();
        const std::uint32_t black =
            format == PixelFormat::kIndexed8 ? 0u : argbToRaw(format, 0xFF000000u);
        const Status filled = Fill(backBuffer_, Rect{}, black);
        if (!filled.Ok()) {
            return filled.Code();
        }
        return backBuffer_;
    }

    Result<BufferId> CreateBuffer(BufferKind kind, std::uint32_t bytes) override {
        if (device_ == nullptr) {
            return Error::kWrongState;
        }
        if (bytes == 0 || (kind == BufferKind::kIndex && bytes % 2 != 0)) {
            return Error::kInvalidArgument;
        }
        if (bytes > kMaxBufferBytes) {
            return Error::kOutOfResources;
        }
        BufferRec rec;
        rec.kind = kind;
        rec.bytes.assign(bytes, 0);
        const BufferId id{NextHandle()};
        buffers_.emplace(id.value, std::move(rec));
        return id;
    }

    Status WriteBuffer(BufferId buffer, std::uint32_t offset, const void *data,
                       std::uint32_t bytes) override {
        auto it = buffers_.find(buffer.value);
        if (it == buffers_.end()) {
            return Error::kInvalidHandle;
        }
        std::vector<std::uint8_t> &store = it->second.bytes;
        if (std::uint64_t{offset} + bytes > store.size() || (bytes != 0 && data == nullptr)) {
            return Error::kInvalidArgument;
        }
        if (bytes != 0) {
            std::memcpy(store.data() + offset, data, bytes);
        }
        return {};
    }

    Status DestroyBuffer(BufferId buffer) override {
        return buffers_.erase(buffer.value) != 0 ? Status{} : Status{Error::kInvalidHandle};
    }

    Result<TextureId> CreateTexture(const TextureDesc &desc) override {
        if (device_ == nullptr) {
            return Error::kWrongState;
        }
        return MakeTexture(desc);
    }

    Status WriteTexture(TextureId texture, std::uint8_t level, Rect region, const void *pixels,
                        std::uint32_t rowPitch) override {
        TextureRec *rec = FindTexture(texture);
        if (rec == nullptr) {
            return Error::kInvalidHandle;
        }
        Rect area;
        const Status checked = CheckTransfer(*rec, texture, level, region, pixels, rowPitch, area);
        if (!checked.Ok()) {
            return checked;
        }
        const auto *src = static_cast<const std::uint8_t *>(pixels);
        if (rec->desc.format == PixelFormat::kIndexed8) {
            const std::uint32_t width = levelSize(rec->desc.width, level);
            std::vector<std::uint8_t> &indices = rec->indices[level];
            for (std::uint32_t row = 0; row < area.height; ++row) {
                std::memcpy(indices.data() + (area.y + row) * width + area.x,
                            src + std::size_t{row} * rowPitch, area.width);
            }
            return Expand(*rec, level, area);
        }
        std::vector<std::uint8_t> bgra(std::size_t{area.width} * area.height * 4);
        for (std::uint32_t row = 0; row < area.height; ++row) {
            toBgra(rec->desc.format, src + std::size_t{row} * rowPitch,
                   bgra.data() + std::size_t{row} * area.width * 4, area.width, nullptr);
        }
        return Upload(*rec, level, area, bgra.data());
    }

    Status ReadTexture(TextureId texture, std::uint8_t level, Rect region, void *out,
                       std::uint32_t rowPitch) override {
        TextureRec *rec = FindTexture(texture);
        if (rec == nullptr) {
            return Error::kInvalidHandle;
        }
        Rect area;
        const Status checked = CheckTransfer(*rec, texture, level, region, out, rowPitch, area);
        if (!checked.Ok()) {
            return checked;
        }
        auto *dst = static_cast<std::uint8_t *>(out);
        if (rec->desc.format == PixelFormat::kIndexed8) {
            const std::uint32_t width = levelSize(rec->desc.width, level);
            const std::vector<std::uint8_t> &indices = rec->indices[level];
            for (std::uint32_t row = 0; row < area.height; ++row) {
                std::memcpy(dst + std::size_t{row} * rowPitch,
                            indices.data() + (area.y + row) * width + area.x, area.width);
            }
            return {};
        }
        std::vector<std::uint8_t> bgra;
        if (!Download(*rec, level, area, bgra)) {
            return Error::kIoError;
        }
        for (std::uint32_t row = 0; row < area.height; ++row) {
            fromBgra(rec->desc.format, bgra.data() + std::size_t{row} * area.width * 4,
                     dst + std::size_t{row} * rowPitch, area.width);
        }
        return {};
    }

    Status DestroyTexture(TextureId texture) override {
        if (FindTexture(texture) == nullptr) {
            return Error::kInvalidHandle;
        }
        if (texture == backBuffer_ || IsSceneTarget(texture)) {
            return Error::kWrongState;
        }
        FlushScene(); // a recorded draw may sample it; SDL frees it once that work is done
        ReleaseTexture(texture);
        return {};
    }

    Status SetColorKey(TextureId texture, std::optional<std::uint32_t> key) override {
        TextureRec *rec = FindTexture(texture);
        if (rec == nullptr) {
            return Error::kInvalidHandle;
        }
        if (rec->desc.format == PixelFormat::kDepth16 ||
            (key.has_value() && (*key & ~formatMask(rec->desc.format)) != 0)) {
            return Error::kInvalidArgument;
        }
        rec->key = key;
        return {};
    }

    Result<PaletteId> CreatePalette(const PaletteEntry *entries) override {
        if (device_ == nullptr) {
            return Error::kWrongState;
        }
        PaletteRec rec;
        if (entries != nullptr) {
            std::copy(entries, entries + kPaletteSize, rec.entries.begin());
        }
        const PaletteId id{NextHandle()};
        palettes_.emplace(id.value, rec);
        return id;
    }

    Status WritePalette(PaletteId palette, std::uint32_t first, std::uint32_t count,
                        const PaletteEntry *entries) override {
        auto it = palettes_.find(palette.value);
        if (it == palettes_.end()) {
            return Error::kInvalidHandle;
        }
        if (std::uint64_t{first} + count > kPaletteSize || (count != 0 && entries == nullptr)) {
            return Error::kInvalidArgument;
        }
        std::copy(entries, entries + count, it->second.entries.begin() + first);
        return RefreshPaletteUsers(palette);
    }

    Status DestroyPalette(PaletteId palette) override {
        if (palettes_.erase(palette.value) == 0) {
            return Error::kInvalidHandle;
        }
        return RefreshPaletteUsers(palette, true);
    }

    Status AttachPalette(TextureId texture, PaletteId palette) override {
        TextureRec *rec = FindTexture(texture);
        if (rec == nullptr || (palette.Valid() && palettes_.count(palette.value) == 0)) {
            return Error::kInvalidHandle;
        }
        if (rec->desc.format != PixelFormat::kIndexed8) {
            return Error::kInvalidArgument;
        }
        rec->palette = palette;
        return ExpandAll(*rec);
    }

    Result<PipelineId> CreatePipeline(const PipelineDesc &desc) override {
        if (device_ == nullptr) {
            return Error::kWrongState;
        }
        if (!ValidPipeline(desc)) {
            return Error::kInvalidArgument;
        }
        const std::string key = pipelineKey(desc, true, true);
        auto cached = pipelineIds_.find(key);
        if (cached != pipelineIds_.end()) {
            ++pipelines_[cached->second].references;
            return PipelineId{cached->second};
        }
        SDL_GPUGraphicsPipeline *gpu = BuildPipeline(desc, true, true);
        if (gpu == nullptr) {
            return Error::kOutOfResources;
        }
        PipelineRec rec;
        rec.desc = desc;
        rec.gpu = gpu;
        rec.key = key;
        rec.references = 1;
        const PipelineId id{NextHandle()};
        pipelines_.emplace(id.value, std::move(rec));
        pipelineIds_.emplace(key, id.value);
        return id;
    }

    Status DestroyPipeline(PipelineId pipeline) override {
        auto it = pipelines_.find(pipeline.value);
        if (it == pipelines_.end()) {
            return Error::kInvalidHandle;
        }
        if (--it->second.references == 0) {
            if (inScene_) {
                FlushScene(); // recorded draws still use it
            }
            SDL_ReleaseGPUGraphicsPipeline(device_, it->second.gpu);
            pipelineIds_.erase(it->second.key);
            pipelines_.erase(it);
        }
        return {};
    }

    Status BeginScene(const RenderTargets &targets) override {
        if (device_ == nullptr || inScene_) {
            return Error::kWrongState;
        }
        TextureRec *color = FindTexture(targets.color);
        TextureRec *depth = targets.depth.Valid() ? FindTexture(targets.depth) : nullptr;
        if (color == nullptr || (targets.depth.Valid() && depth == nullptr)) {
            return Error::kInvalidHandle;
        }
        if (!color->desc.renderTarget || !isColorFormat(color->desc.format)) {
            return Error::kInvalidArgument;
        }
        if (depth != nullptr &&
            (!depth->desc.renderTarget || depth->desc.format != PixelFormat::kDepth16 ||
             depth->desc.width != color->desc.width || depth->desc.height != color->desc.height)) {
            return Error::kInvalidArgument;
        }
        sceneCommands_ = SDL_AcquireGPUCommandBuffer(device_);
        if (sceneCommands_ == nullptr) {
            return Error::kOutOfResources;
        }
        inScene_ = true;
        sceneTargets_ = targets;
        scene_ = Pass{};
        scene_.color = color->gpu;
        scene_.depth = depth != nullptr ? depth->gpu : nullptr;
        scene_.width = color->desc.width;
        scene_.height = color->desc.height;
        sceneFormat_ = color->desc.format;
        viewport_ = SDL_Rect{0, 0, static_cast<int>(scene_.width), static_cast<int>(scene_.height)};
        viewportMinDepth_ = 0.0f;
        viewportMaxDepth_ = 1.0f;
        return {};
    }

    Status SetViewport(const Viewport &viewport) override {
        if (!inScene_) {
            return Error::kWrongState;
        }
        Rect area;
        if (!resolveRect(Rect{viewport.x, viewport.y, viewport.width, viewport.height},
                         scene_.width, scene_.height, area) ||
            !(viewport.minDepth >= 0.0f && viewport.maxDepth <= 1.0f &&
              viewport.minDepth <= viewport.maxDepth)) {
            return Error::kInvalidArgument;
        }
        viewport_ = SDL_Rect{area.x, area.y, static_cast<int>(area.width),
                             static_cast<int>(area.height)};
        viewportMinDepth_ = viewport.minDepth;
        viewportMaxDepth_ = viewport.maxDepth;
        return {};
    }

    Status Clear(const ClearDesc &clear) override {
        if (!inScene_ || (clear.depth && scene_.depth == nullptr)) {
            return Error::kWrongState;
        }
        if (!clear.color && !clear.depth) {
            return {};
        }
        SDL_Rect area = viewport_;
        const Rect &a = clear.area;
        if (a.x != 0 || a.y != 0 || a.width != 0 || a.height != 0) {
            Rect resolved;
            if (!resolveRect(a, scene_.width, scene_.height, resolved)) {
                return {};
            }
            const SDL_Rect wanted{resolved.x, resolved.y, static_cast<int>(resolved.width),
                                  static_cast<int>(resolved.height)};
            if (!SDL_GetRectIntersection(&wanted, &viewport_, &area)) {
                return {};
            }
        }
        PipelineDesc desc = quadPipeline(TextureArg::kDiffuse);
        desc.depthTarget = scene_.depth != nullptr;
        desc.depthTest = clear.depth;
        desc.depthWrite = clear.depth;
        desc.depthFunction = CompareFunction::kAlways;
        SDL_GPUGraphicsPipeline *pipeline = InternalPipeline(desc, true, clear.color);
        if (pipeline == nullptr) {
            return Error::kOutOfResources;
        }
        RecordQuad(scene_, pipeline, area, clear.depthValue, clear.colorValue, nullptr, 0, 0,
                   0, 0, std::nullopt);
        return {};
    }

    Status Draw(const DrawCall &draw) override {
        if (!inScene_) {
            return Error::kWrongState;
        }
        auto pit = pipelines_.find(draw.pipeline.value);
        auto vit = buffers_.find(draw.vertices.value);
        if (pit == pipelines_.end() || vit == buffers_.end() ||
            vit->second.kind != BufferKind::kVertex) {
            return Error::kInvalidHandle;
        }
        const PipelineDesc &desc = pit->second.desc;
        if (desc.colorFormat != sceneFormat_ || desc.depthTarget != (scene_.depth != nullptr)) {
            return Error::kWrongState;
        }
        const BufferRec *indexBuffer = nullptr;
        if (draw.indices.Valid()) {
            auto iit = buffers_.find(draw.indices.value);
            if (iit == buffers_.end() || iit->second.kind != BufferKind::kIndex) {
                return Error::kInvalidHandle;
            }
            indexBuffer = &iit->second;
        }
        Command command;
        const Status bound = BindTextures(desc, draw.state, command);
        if (!bound.Ok()) {
            return bound;
        }
        const Status staged = Stage(desc, draw, vit->second, indexBuffer, command);
        if (!staged.Ok()) {
            return staged;
        }
        command.pipeline = pit->second.gpu;
        command.scissor = viewport_;
        command.minDepth = viewportMinDepth_;
        command.maxDepth = viewportMaxDepth_;
        SetUniforms(desc, draw.state, command);
        scene_.commands.push_back(command);
        return {};
    }

    Status EndScene() override {
        if (!inScene_) {
            return Error::kWrongState;
        }
        FlushScene();
        SDL_SubmitGPUCommandBuffer(sceneCommands_);
        sceneCommands_ = nullptr;
        inScene_ = false;
        sceneTargets_ = RenderTargets{};
        scene_ = Pass{};
        return {};
    }

    Status Blit(const BlitDesc &blit) override {
        if (inScene_) {
            return Error::kWrongState;
        }
        TextureRec *src = FindTexture(blit.source);
        TextureRec *dst = FindTexture(blit.destination);
        if (src == nullptr || dst == nullptr) {
            return Error::kInvalidHandle;
        }
        const PixelFormat sf = src->desc.format;
        const PixelFormat df = dst->desc.format;
        const bool formatsFit =
            sf == df || (sf == PixelFormat::kIndexed8 && isColorFormat(df));
        Rect from;
        Rect to;
        if (!formatsFit || sf == PixelFormat::kDepth16 || df == PixelFormat::kDepth16 ||
            !resolveRect(blit.sourceRect, src->desc.width, src->desc.height, from) ||
            !resolveRect(blit.destinationRect, dst->desc.width, dst->desc.height, to)) {
            return Error::kInvalidArgument;
        }
        if (blit.source == blit.destination && Overlap(from, to)) {
            return Error::kInvalidArgument;
        }
        const std::optional<std::uint32_t> key =
            blit.sourceColorKey ? src->key : std::optional<std::uint32_t>{};
        if (df == PixelFormat::kIndexed8) {
            return BlitIndexed(*src, from, *dst, to, key);
        }
        return BlitGpu(*src, from, *dst, to, key);
    }

    Status Fill(TextureId texture, Rect area, std::uint32_t value) override {
        if (inScene_) {
            return Error::kWrongState;
        }
        TextureRec *rec = FindTexture(texture);
        if (rec == nullptr) {
            return Error::kInvalidHandle;
        }
        Rect target;
        if (!resolveRect(area, rec->desc.width, rec->desc.height, target)) {
            return Error::kInvalidArgument;
        }
        const PixelFormat format = rec->desc.format;
        value &= formatMask(format);
        if (format == PixelFormat::kDepth16) {
            return FillDepth(*rec, target, static_cast<float>(value) / 65535.0f);
        }
        if (format == PixelFormat::kIndexed8) {
            std::vector<std::uint8_t> &indices = rec->indices[0];
            for (std::uint32_t row = 0; row < target.height; ++row) {
                std::memset(indices.data() + (target.y + row) * rec->desc.width + target.x,
                            static_cast<int>(value), target.width);
            }
            return Expand(*rec, 0, target);
        }
        const std::uint32_t argb = rawToArgb(format, value, nullptr);
        std::vector<std::uint32_t> block(std::size_t{target.width} * target.height, argb);
        return Upload(*rec, 0, target, reinterpret_cast<const std::uint8_t *>(block.data()));
    }

    Status Present() override {
        if (device_ == nullptr || inScene_ || !backBuffer_.Valid()) {
            return Error::kWrongState;
        }
        const std::uint32_t fps = EffectiveFrameRate();
        bool swapchainAcquired = false;
        if (claimed_) {
            const SDL_GPUPresentMode mode = ChoosePresentMode();
            if (mode != presentMode_ &&
                SDL_SetGPUSwapchainParameters(device_, sdlWindow_,
                                              SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode)) {
                presentMode_ = mode;
            }
            if (!ShowBackBuffer(swapchainAcquired)) {
                return Error::kIoError;
            }
        }
        if (presentNeedsPacing(claimed_, explicitCap_, swapchainAcquired)) {
            Pace(fps);
        }
        return {};
    }

private:
    // --- attachment -------------------------------------------------------------------------

    static SDL_Window *FindSdlWindow(Window &window) {
        int count = 0;
        SDL_Window **windows = SDL_GetWindows(&count);
        SDL_Window *found = nullptr;
        for (int i = 0; windows != nullptr && i < count; ++i) {
            if (SDL_GetPointerProperty(SDL_GetWindowProperties(windows[i]), kWindowProperty,
                                       nullptr) == static_cast<void *>(&window)) {
                found = windows[i];
            }
        }
        SDL_free(windows);
        return found;
    }

    // No device for the shader formats this build carries. If a driver exists that needs a
    // format not compiled in yet, say which; otherwise the device or its driver failed.
    static Status NoDevice() {
        const std::string failure = SDL_GetError();
        const std::uint32_t carried = shaderFormats();
        const std::uint32_t others = kAllShaderFormats & ~carried;
        const int drivers = SDL_GetNumGPUDrivers();
        for (int i = 0; i < drivers; ++i) {
            const char *name = SDL_GetGPUDriver(i);
            if (noDeviceError(SDL_GPUSupportsShaderFormats(carried, name),
                              SDL_GPUSupportsShaderFormats(others, name)) ==
                Error::kUnsupported) {
                SDL_LogWarn(SDL_LOG_CATEGORY_GPU,
                            "The %s GPU driver needs a shader format this build does not carry "
                            "yet; rendering is unsupported here (%s).",
                            name, failure.c_str());
                return Error::kUnsupported;
            }
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_GPU, "No GPU device could be created: %s", failure.c_str());
        return Error::kIoError;
    }

    bool CreateFixedObjects() {
        const std::uint32_t formats = shaderFormats() & SDL_GetGPUShaderFormats(device_);
        std::uint32_t format = 0;
        for (std::uint32_t bit = 1; bit != 0 && format == 0; bit <<= 1) {
            if ((formats & bit) != 0) {
                format = bit;
            }
        }
        vertexShader_ = MakeShader(format, false);
        fragmentShader_ = MakeShader(format, true);
        if (vertexShader_ == nullptr || fragmentShader_ == nullptr) {
            return false;
        }
        SDL_GPUBufferCreateInfo info{};
        info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        info.size = sizeof(DefaultAttributes);
        defaults_ = SDL_CreateGPUBuffer(device_, &info);
        if (defaults_ == nullptr) {
            return false;
        }
        const DefaultAttributes values;
        if (!UploadBuffer(defaults_, &values, sizeof values)) {
            return false;
        }
        SDL_GPUTextureCreateInfo tex{};
        tex.type = SDL_GPU_TEXTURETYPE_2D;
        tex.format = kColorFormat;
        tex.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        tex.width = 1;
        tex.height = 1;
        tex.layer_count_or_depth = 1;
        tex.num_levels = 1;
        white_ = SDL_CreateGPUTexture(device_, &tex);
        if (white_ == nullptr) {
            return false;
        }
        TextureRec rec;
        rec.gpu = white_;
        rec.desc.width = 1;
        rec.desc.height = 1;
        const std::uint32_t pixel = 0xFFFFFFFFu;
        return Upload(rec, 0, Rect{0, 0, 1, 1}, reinterpret_cast<const std::uint8_t *>(&pixel))
            .Ok();
    }

    SDL_GPUShader *MakeShader(std::uint32_t format, bool fragment) {
        const std::uint8_t *code = nullptr;
        std::size_t size = 0;
        const char *entry = nullptr;
        if (!shaderCode(format, fragment, code, size, entry)) {
            return nullptr;
        }
        SDL_GPUShaderCreateInfo info{};
        info.code = code;
        info.code_size = size;
        info.entrypoint = entry;
        info.format = format;
        info.stage = fragment ? SDL_GPU_SHADERSTAGE_FRAGMENT : SDL_GPU_SHADERSTAGE_VERTEX;
        info.num_samplers = fragment ? static_cast<Uint32>(kTextureStages) : 0;
        info.num_uniform_buffers = 1;
        return SDL_CreateGPUShader(device_, &info);
    }

    void ReleaseAll() {
        if (device_ == nullptr) {
            return;
        }
        if (inScene_) {
            FlushScene();
            SDL_SubmitGPUCommandBuffer(sceneCommands_);
            sceneCommands_ = nullptr;
            inScene_ = false;
        }
        SDL_WaitForGPUIdle(device_);
        for (auto &entry : textures_) {
            SDL_ReleaseGPUTexture(device_, entry.second.gpu);
        }
        for (auto &entry : pipelines_) {
            SDL_ReleaseGPUGraphicsPipeline(device_, entry.second.gpu);
        }
        for (auto &entry : internalPipelines_) {
            SDL_ReleaseGPUGraphicsPipeline(device_, entry.second);
        }
        for (auto &entry : samplers_) {
            SDL_ReleaseGPUSampler(device_, entry.second);
        }
        textures_.clear();
        buffers_.clear();
        palettes_.clear();
        pipelines_.clear();
        pipelineIds_.clear();
        internalPipelines_.clear();
        samplers_.clear();
        backBuffer_ = TextureId{};
        SDL_ReleaseGPUTexture(device_, white_);
        SDL_ReleaseGPUBuffer(device_, defaults_);
        SDL_ReleaseGPUBuffer(device_, stream_);
        SDL_ReleaseGPUTransferBuffer(device_, streamUpload_);
        SDL_ReleaseGPUShader(device_, vertexShader_);
        SDL_ReleaseGPUShader(device_, fragmentShader_);
        white_ = nullptr;
        defaults_ = nullptr;
        stream_ = nullptr;
        streamUpload_ = nullptr;
        streamSize_ = 0;
        vertexShader_ = nullptr;
        fragmentShader_ = nullptr;
        if (claimed_) {
            SDL_ReleaseWindowFromGPUDevice(device_, sdlWindow_);
            claimed_ = false;
        }
        SDL_DestroyGPUDevice(device_);
        device_ = nullptr;
        window_ = nullptr;
        sdlWindow_ = nullptr;
        presentMode_ = SDL_GPU_PRESENTMODE_VSYNC;
        paced_ = false;
    }

    std::uint32_t NextHandle() { return nextHandle_++; }

    // --- textures ---------------------------------------------------------------------------

    TextureRec *FindTexture(TextureId id) {
        auto it = textures_.find(id.value);
        return it == textures_.end() ? nullptr : &it->second;
    }

    bool IsSceneTarget(TextureId id) const {
        return inScene_ && (id == sceneTargets_.color || id == sceneTargets_.depth);
    }

    Result<TextureId> MakeTexture(const TextureDesc &desc) {
        const std::uint32_t smaller = (std::min)(desc.width, desc.height);
        std::uint32_t maxLevels = 0;
        for (std::uint32_t s = smaller; s != 0; s >>= 1) {
            ++maxLevels;
        }
        const auto format = static_cast<std::uint32_t>(desc.format);
        if (desc.width == 0 || desc.height == 0 || desc.width > kMaxTextureSize ||
            desc.height > kMaxTextureSize || desc.mipLevels == 0 ||
            desc.mipLevels > maxLevels || format > static_cast<std::uint32_t>(PixelFormat::kDepth16) ||
            (desc.format == PixelFormat::kDepth16 && !desc.renderTarget)) {
            return Error::kInvalidArgument;
        }
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.width = desc.width;
        info.height = desc.height;
        info.layer_count_or_depth = 1;
        info.num_levels = desc.mipLevels;
        if (desc.format == PixelFormat::kDepth16) {
            info.format = kDepthFormat;
            info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        } else {
            info.format = kColorFormat;
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        }
        SDL_GPUTexture *gpu = SDL_CreateGPUTexture(device_, &info);
        if (gpu == nullptr) {
            return Error::kOutOfResources;
        }
        TextureRec rec;
        rec.desc = desc;
        rec.gpu = gpu;
        if (desc.format == PixelFormat::kIndexed8) {
            for (std::uint8_t level = 0; level < desc.mipLevels; ++level) {
                rec.indices.emplace_back(std::size_t{levelSize(desc.width, level)} *
                                             levelSize(desc.height, level),
                                         std::uint8_t{0});
            }
        }
        const TextureId id{NextHandle()};
        textures_.emplace(id.value, std::move(rec));
        return id;
    }

    void ReleaseTexture(TextureId id) {
        auto it = textures_.find(id.value);
        if (it == textures_.end()) {
            return;
        }
        SDL_ReleaseGPUTexture(device_, it->second.gpu);
        textures_.erase(it);
    }

    Status CheckTransfer(const TextureRec &rec, TextureId id, std::uint8_t level, const Rect &in,
                         const void *pixels, std::uint32_t rowPitch, Rect &area) const {
        if (rec.desc.format == PixelFormat::kDepth16 || level >= rec.desc.mipLevels ||
            pixels == nullptr ||
            !resolveRect(in, levelSize(rec.desc.width, level), levelSize(rec.desc.height, level),
                         area) ||
            std::uint64_t{rowPitch} < std::uint64_t{area.width} * pixelBytes(rec.desc.format)) {
            return Error::kInvalidArgument;
        }
        if (inScene_ && id == sceneTargets_.color) {
            return Error::kWrongState;
        }
        return {};
    }

    // Re-expands a region of an indexed texture through its palette and uploads it.
    Status Expand(TextureRec &rec, std::uint8_t level, const Rect &area) {
        const PaletteEntry *palette = nullptr;
        auto it = palettes_.find(rec.palette.value);
        if (it != palettes_.end()) {
            palette = it->second.entries.data();
        }
        const std::uint32_t width = levelSize(rec.desc.width, level);
        std::vector<std::uint8_t> bgra(std::size_t{area.width} * area.height * 4);
        for (std::uint32_t row = 0; row < area.height; ++row) {
            toBgra(PixelFormat::kIndexed8,
                   rec.indices[level].data() + (area.y + row) * width + area.x,
                   bgra.data() + std::size_t{row} * area.width * 4, area.width, palette);
        }
        return Upload(rec, level, area, bgra.data());
    }

    Status ExpandAll(TextureRec &rec) {
        for (std::uint8_t level = 0; level < rec.desc.mipLevels; ++level) {
            const Status status =
                Expand(rec, level,
                       Rect{0, 0, levelSize(rec.desc.width, level),
                            levelSize(rec.desc.height, level)});
            if (!status.Ok()) {
                return status;
            }
        }
        return {};
    }

    Status RefreshPaletteUsers(PaletteId palette, bool detach = false) {
        for (auto &entry : textures_) {
            TextureRec &rec = entry.second;
            if (rec.palette == palette) {
                if (detach) {
                    rec.palette = PaletteId{};
                }
                const Status status = ExpandAll(rec);
                if (!status.Ok()) {
                    return status;
                }
            }
        }
        return {};
    }

    // --- transfers --------------------------------------------------------------------------

    // The command buffer for a copy or pass outside the recorded scene: the scene's own (after
    // encoding what it has recorded, so the order holds) or a new one.
    SDL_GPUCommandBuffer *BeginWork() {
        if (inScene_) {
            FlushScene();
            return sceneCommands_;
        }
        return SDL_AcquireGPUCommandBuffer(device_);
    }

    void EndWork(SDL_GPUCommandBuffer *commands) {
        if (!inScene_) {
            SDL_SubmitGPUCommandBuffer(commands);
        }
    }

    bool UploadBuffer(SDL_GPUBuffer *buffer, const void *data, std::uint32_t bytes) {
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = bytes;
        SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(device_, &info);
        if (transfer == nullptr) {
            return false;
        }
        void *mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
        if (mapped == nullptr) {
            SDL_ReleaseGPUTransferBuffer(device_, transfer);
            return false;
        }
        std::memcpy(mapped, data, bytes);
        SDL_UnmapGPUTransferBuffer(device_, transfer);
        SDL_GPUCommandBuffer *commands = BeginWork();
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
        SDL_GPUTransferBufferLocation from{};
        from.transfer_buffer = transfer;
        SDL_GPUBufferRegion to{};
        to.buffer = buffer;
        to.size = bytes;
        SDL_UploadToGPUBuffer(copy, &from, &to, false);
        SDL_EndGPUCopyPass(copy);
        EndWork(commands);
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        return true;
    }

    // Uploads B, G, R, A pixels, tightly packed, into a region of one level.
    Status Upload(TextureRec &rec, std::uint8_t level, const Rect &area, const std::uint8_t *bgra) {
        const std::uint32_t bytes = area.width * area.height * 4;
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = bytes;
        SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(device_, &info);
        if (transfer == nullptr) {
            return Error::kOutOfResources;
        }
        void *mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
        if (mapped == nullptr) {
            SDL_ReleaseGPUTransferBuffer(device_, transfer);
            return Error::kOutOfResources;
        }
        std::memcpy(mapped, bgra, bytes);
        SDL_UnmapGPUTransferBuffer(device_, transfer);
        SDL_GPUCommandBuffer *commands = BeginWork();
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
        SDL_GPUTextureTransferInfo from{};
        from.transfer_buffer = transfer;
        from.pixels_per_row = area.width;
        from.rows_per_layer = area.height;
        SDL_GPUTextureRegion to{};
        to.texture = rec.gpu;
        to.mip_level = level;
        to.x = static_cast<Uint32>(area.x);
        to.y = static_cast<Uint32>(area.y);
        to.w = area.width;
        to.h = area.height;
        to.d = 1;
        SDL_UploadToGPUTexture(copy, &from, &to, false);
        SDL_EndGPUCopyPass(copy);
        EndWork(commands);
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        return {};
    }

    // Reads a region of one level back as B, G, R, A pixels, waiting for the work before it.
    bool Download(TextureRec &rec, std::uint8_t level, const Rect &area,
                  std::vector<std::uint8_t> &bgra) {
        if (inScene_) {
            FlushScene();
            SDL_SubmitGPUCommandBuffer(sceneCommands_);
            sceneCommands_ = SDL_AcquireGPUCommandBuffer(device_);
        }
        const std::uint32_t bytes = area.width * area.height * 4;
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        info.size = bytes;
        SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(device_, &info);
        if (transfer == nullptr) {
            return false;
        }
        SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device_);
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
        SDL_GPUTextureRegion from{};
        from.texture = rec.gpu;
        from.mip_level = level;
        from.x = static_cast<Uint32>(area.x);
        from.y = static_cast<Uint32>(area.y);
        from.w = area.width;
        from.h = area.height;
        from.d = 1;
        SDL_GPUTextureTransferInfo to{};
        to.transfer_buffer = transfer;
        to.pixels_per_row = area.width;
        to.rows_per_layer = area.height;
        SDL_DownloadFromGPUTexture(copy, &from, &to);
        SDL_EndGPUCopyPass(copy);
        SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
        bool ok = fence != nullptr && SDL_WaitForGPUFences(device_, true, &fence, 1);
        SDL_ReleaseGPUFence(device_, fence);
        if (ok) {
            const void *mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
            ok = mapped != nullptr;
            if (ok) {
                bgra.assign(static_cast<const std::uint8_t *>(mapped),
                            static_cast<const std::uint8_t *>(mapped) + bytes);
                SDL_UnmapGPUTransferBuffer(device_, transfer);
            }
        }
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        return ok;
    }

    // --- pipelines --------------------------------------------------------------------------

    static bool ValidPipeline(const PipelineDesc &desc) {
        if (!isColorFormat(desc.colorFormat) ||
            static_cast<std::uint32_t>(desc.colorFormat) >
                static_cast<std::uint32_t>(PixelFormat::kDepth16) ||
            desc.layout.texCoordSets > 2 ||
            static_cast<std::uint32_t>(desc.primitive) >
                static_cast<std::uint32_t>(PrimitiveType::kTriangleFan)) {
            return false;
        }
        const std::uint32_t sets = (std::max)(1u, std::uint32_t{desc.layout.texCoordSets});
        bool disabled = false;
        for (const TextureStage &stage : desc.stages) {
            const bool enabled = stage.colorOp != TextureOp::kDisable;
            if (enabled && (disabled || stage.texCoordIndex >= sets)) {
                return false;
            }
            disabled = disabled || !enabled;
        }
        return true;
    }

    SDL_GPUGraphicsPipeline *InternalPipeline(const PipelineDesc &desc, bool colorTarget,
                                              bool colorWrite) {
        const std::string key = pipelineKey(desc, colorTarget, colorWrite);
        auto it = internalPipelines_.find(key);
        if (it != internalPipelines_.end()) {
            return it->second;
        }
        SDL_GPUGraphicsPipeline *pipeline = BuildPipeline(desc, colorTarget, colorWrite);
        if (pipeline != nullptr) {
            internalPipelines_.emplace(key, pipeline);
        }
        return pipeline;
    }

    SDL_GPUGraphicsPipeline *BuildPipeline(const PipelineDesc &desc, bool colorTarget,
                                           bool colorWrite) {
        const VertexLayout &layout = desc.layout;
        SDL_GPUVertexBufferDescription buffers[2]{};
        buffers[0].slot = 0;
        buffers[0].pitch = vertexStride(layout);
        buffers[0].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        buffers[1].slot = 1;
        buffers[1].pitch = sizeof(DefaultAttributes);
        buffers[1].input_rate = SDL_GPU_VERTEXINPUTRATE_INSTANCE;
        SDL_GPUVertexAttribute attributes[5]{};
        std::uint32_t offset = layout.position == PositionKind::kXyzRhw ? 16u : 12u;
        attributes[0].location = 0;
        attributes[0].format = layout.position == PositionKind::kXyzRhw
                                   ? SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4
                                   : SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        if (layout.normal) {
            offset += 12;
        }
        const auto place = [&](int index, bool present, SDL_GPUVertexElementFormat format,
                               std::uint32_t size, std::uint32_t fallback) {
            attributes[index].location = static_cast<Uint32>(index);
            attributes[index].format = format;
            if (present) {
                attributes[index].offset = offset;
                offset += size;
            } else {
                attributes[index].buffer_slot = 1;
                attributes[index].offset = fallback;
            }
        };
        place(1, layout.diffuse, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 4, 0);
        place(2, layout.specular, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 4, 4);
        place(3, layout.texCoordSets >= 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 8, 8);
        place(4, layout.texCoordSets >= 2, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 8, 8);

        SDL_GPUColorTargetDescription target{};
        target.format = kColorFormat;
        target.blend_state.enable_blend = desc.blend;
        target.blend_state.src_color_blendfactor = toSdl(desc.sourceBlend);
        target.blend_state.dst_color_blendfactor = toSdl(desc.destinationBlend);
        target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        target.blend_state.src_alpha_blendfactor = toSdl(desc.sourceBlend);
        target.blend_state.dst_alpha_blendfactor = toSdl(desc.destinationBlend);
        target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        target.blend_state.enable_color_write_mask = !colorWrite;
        target.blend_state.color_write_mask = 0;

        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = vertexShader_;
        info.fragment_shader = fragmentShader_;
        info.vertex_input_state.vertex_buffer_descriptions = buffers;
        info.vertex_input_state.num_vertex_buffers = 2;
        info.vertex_input_state.vertex_attributes = attributes;
        info.vertex_input_state.num_vertex_attributes = 5;
        info.primitive_type = toSdl(desc.primitive);
        info.rasterizer_state.fill_mode =
            desc.fill == FillMode::kSolid ? SDL_GPU_FILLMODE_FILL : SDL_GPU_FILLMODE_LINE;
        // On screen, Direct3D culls by the winding the viewer sees; front faces are clockwise.
        info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
        info.rasterizer_state.cull_mode = desc.cull == CullMode::kNone ? SDL_GPU_CULLMODE_NONE
                                          : desc.cull == CullMode::kClockwise
                                              ? SDL_GPU_CULLMODE_FRONT
                                              : SDL_GPU_CULLMODE_BACK;
        info.rasterizer_state.enable_depth_clip = true;
        info.depth_stencil_state.enable_depth_test = desc.depthTest;
        info.depth_stencil_state.enable_depth_write = desc.depthTest && desc.depthWrite;
        info.depth_stencil_state.compare_op = toSdl(desc.depthFunction);
        info.target_info.color_target_descriptions = &target;
        info.target_info.num_color_targets = colorTarget ? 1 : 0;
        info.target_info.has_depth_stencil_target = desc.depthTarget;
        info.target_info.depth_stencil_format = kDepthFormat;
        return SDL_CreateGPUGraphicsPipeline(device_, &info);
    }

    SDL_GPUSampler *Sampler(const TextureStage &stage) {
        const std::uint32_t key = samplerKey(stage);
        auto it = samplers_.find(key);
        if (it != samplers_.end()) {
            return it->second;
        }
        SDL_GPUSamplerCreateInfo info{};
        info.mag_filter = toSdl(stage.magFilter);
        info.min_filter = toSdl(stage.minFilter);
        info.mipmap_mode = stage.mipFilter == MipFilter::kLinear
                               ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR
                               : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        info.address_mode_u = toSdl(stage.addressU);
        info.address_mode_v = toSdl(stage.addressV);
        info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        info.min_lod = 0.0f;
        info.max_lod = stage.mipFilter == MipFilter::kNone ? 0.0f : 1000.0f;
        SDL_GPUSampler *sampler = SDL_CreateGPUSampler(device_, &info);
        if (sampler != nullptr) {
            samplers_.emplace(key, sampler);
        }
        return sampler;
    }

    // --- drawing ----------------------------------------------------------------------------

    Status BindTextures(const PipelineDesc &desc, const DrawState &state, Command &command) {
        for (std::size_t s = 0; s < kTextureStages; ++s) {
            const bool enabled = desc.stages[s].colorOp != TextureOp::kDisable;
            const TextureId id = state.textures[s];
            if (!enabled && id.Valid()) {
                return Error::kInvalidArgument;
            }
            SDL_GPUTexture *texture = white_;
            if (id.Valid()) {
                const TextureRec *rec = FindTexture(id);
                if (rec == nullptr) {
                    return Error::kInvalidHandle;
                }
                if (rec->desc.format == PixelFormat::kDepth16 || IsSceneTarget(id)) {
                    return Error::kInvalidArgument;
                }
                texture = rec->gpu;
            }
            command.textures[s] = texture;
            command.samplers[s] = Sampler(desc.stages[s]);
            if (command.samplers[s] == nullptr) {
                return Error::kOutOfResources;
            }
        }
        return {};
    }

    static std::uint32_t Align(std::vector<std::uint8_t> &staging) {
        const std::size_t aligned = (staging.size() + 15) & ~std::size_t{15};
        staging.resize(aligned);
        return static_cast<std::uint32_t>(aligned);
    }

    // Copies the draw's vertices (and indices, a fan converted to a list) into the scene.
    Status Stage(const PipelineDesc &desc, const DrawCall &draw, const BufferRec &vertices,
                 const BufferRec *indices, Command &command) {
        const std::uint32_t stride = vertexStride(desc.layout);
        const std::uint64_t vertexEnd = (std::uint64_t{draw.firstVertex} + draw.vertexCount) * stride;
        const std::uint32_t count = indices != nullptr ? draw.indexCount : draw.vertexCount;
        if (draw.vertexCount == 0 || vertexEnd > vertices.bytes.size() ||
            !wholePrimitives(desc.primitive, count)) {
            return Error::kInvalidArgument;
        }
        std::vector<std::uint16_t> list;
        if (indices != nullptr) {
            const std::uint64_t indexEnd = (std::uint64_t{draw.firstIndex} + draw.indexCount) * 2;
            if (indexEnd > indices->bytes.size()) {
                return Error::kInvalidArgument;
            }
            list.resize(draw.indexCount);
            std::memcpy(list.data(), indices->bytes.data() + std::size_t{draw.firstIndex} * 2,
                        std::size_t{draw.indexCount} * 2);
            for (std::uint16_t index : list) {
                if (index >= draw.vertexCount) {
                    return Error::kInvalidArgument;
                }
            }
        }
        if (desc.primitive == PrimitiveType::kTriangleFan) {
            // Indices are 16 bits: a fan of vertices numbered past 65535 cannot be expressed.
            if (indices == nullptr && count > 65536) {
                return Error::kInvalidArgument;
            }
            std::vector<std::uint16_t> fan;
            for (std::uint32_t i = 1; i + 1 < count; ++i) {
                fan.push_back(indices != nullptr ? list[0] : 0);
                fan.push_back(indices != nullptr ? list[i] : static_cast<std::uint16_t>(i));
                fan.push_back(indices != nullptr ? list[i + 1]
                                                 : static_cast<std::uint16_t>(i + 1));
            }
            list = std::move(fan);
        }
        std::vector<std::uint8_t> &staging = scene_.staging;
        command.vertexOffset = Align(staging);
        command.vertexCount = draw.vertexCount;
        const std::uint8_t *first = vertices.bytes.data() + std::size_t{draw.firstVertex} * stride;
        staging.insert(staging.end(), first, first + std::size_t{draw.vertexCount} * stride);
        if (!list.empty()) {
            command.indexed = true;
            command.indexOffset = Align(staging);
            command.indexCount = static_cast<std::uint32_t>(list.size());
            const auto *bytes = reinterpret_cast<const std::uint8_t *>(list.data());
            staging.insert(staging.end(), bytes, bytes + list.size() * 2);
        }
        return {};
    }

    void SetUniforms(const PipelineDesc &desc, const DrawState &state, Command &command) {
        VertexUniforms &vu = command.vertexUniforms;
        vu.target[0] = static_cast<float>(scene_.width);
        vu.target[1] = static_cast<float>(scene_.height);
        vu.target[2] = 1.0f / static_cast<float>(scene_.width);
        vu.target[3] = 1.0f / static_cast<float>(scene_.height);
        vu.select[0] = desc.stages[0].texCoordIndex;
        vu.select[1] = desc.stages[1].texCoordIndex;

        FragmentUniforms &fu = command.fragmentUniforms;
        std::int32_t *stageA[2] = {fu.stage0a, fu.stage1a};
        std::int32_t *stageB[2] = {fu.stage0b, fu.stage1b};
        float *keys[2] = {fu.key0, fu.key1};
        for (std::size_t s = 0; s < kTextureStages; ++s) {
            const TextureStage &stage = desc.stages[s];
            stageA[s][0] = static_cast<std::int32_t>(stage.colorOp);
            stageA[s][1] = static_cast<std::int32_t>(stage.colorArg1);
            stageA[s][2] = static_cast<std::int32_t>(stage.colorArg2);
            stageA[s][3] = static_cast<std::int32_t>(stage.alphaOp);
            stageB[s][0] = static_cast<std::int32_t>(stage.alphaArg1);
            stageB[s][1] = static_cast<std::int32_t>(stage.alphaArg2);
            const TextureRec *rec = FindTexture(state.textures[s]);
            if (desc.colorKey && rec != nullptr && rec->key.has_value()) {
                stageB[s][2] = 1;
                argbToFloats(KeyColor(*rec, *rec->key), keys[s]);
            }
        }
        argbToFloats(state.textureFactor, fu.textureFactor);
        argbToFloats(0xFF000000u | state.fogColor, fu.fogColor);
        fu.fog[0] = state.fogStart;
        fu.fog[1] = state.fogEnd;
        fu.fog[2] = state.fogDensity;
        fu.fog[3] = static_cast<float>(desc.fog);
        fu.misc[0] = desc.alphaTest ? static_cast<std::int32_t>(desc.alphaFunction)
                                    : static_cast<std::int32_t>(CompareFunction::kAlways);
        fu.misc[1] = state.alphaReference;
        fu.misc[2] = desc.specular ? 1 : 0;
        fu.misc[3] = desc.shade == ShadeMode::kFlat ? 1 : 0;
    }

    std::uint32_t KeyColor(const TextureRec &rec, std::uint32_t key) const {
        const PaletteEntry *palette = nullptr;
        auto it = palettes_.find(rec.palette.value);
        if (it != palettes_.end()) {
            palette = it->second.entries.data();
        }
        return rawToArgb(rec.desc.format, key, palette);
    }

    // Records a screen quad covering `area` of the pass's target: a clear when `source` is null,
    // otherwise a copy of the source rectangle (in normalised coordinates).
    void RecordQuad(Pass &pass, SDL_GPUGraphicsPipeline *pipeline, const SDL_Rect &area,
                    float depth, std::uint32_t color, SDL_GPUTexture *source, float u0, float v0,
                    float u1, float v1, std::optional<std::uint32_t> keyColor) {
        const float x0 = static_cast<float>(area.x) - 0.5f;
        const float y0 = static_cast<float>(area.y) - 0.5f;
        const float x1 = x0 + static_cast<float>(area.w);
        const float y1 = y0 + static_cast<float>(area.h);
        const QuadVertex quad[4] = {
            {x0, y0, depth, 1.0f, color, 0xFF000000u, u0, v0},
            {x1, y0, depth, 1.0f, color, 0xFF000000u, u1, v0},
            {x0, y1, depth, 1.0f, color, 0xFF000000u, u0, v1},
            {x1, y1, depth, 1.0f, color, 0xFF000000u, u1, v1},
        };
        Command command;
        command.pipeline = pipeline;
        command.scissor = area;
        command.vertexOffset = Align(pass.staging);
        command.vertexCount = 4;
        const auto *bytes = reinterpret_cast<const std::uint8_t *>(quad);
        pass.staging.insert(pass.staging.end(), bytes, bytes + sizeof quad);
        const PipelineDesc desc =
            quadPipeline(source != nullptr ? TextureArg::kTexture : TextureArg::kDiffuse);
        TextureStage pointClamp = desc.stages[0];
        command.textures = {source != nullptr ? source : white_, white_};
        command.samplers = {Sampler(pointClamp), Sampler(pointClamp)};
        VertexUniforms &vu = command.vertexUniforms;
        vu.target[0] = static_cast<float>(pass.width);
        vu.target[1] = static_cast<float>(pass.height);
        vu.target[2] = 1.0f / static_cast<float>(pass.width);
        vu.target[3] = 1.0f / static_cast<float>(pass.height);
        FragmentUniforms &fu = command.fragmentUniforms;
        fu.stage0a[0] = static_cast<std::int32_t>(TextureOp::kSelectArg1);
        fu.stage0a[1] = static_cast<std::int32_t>(desc.stages[0].colorArg1);
        fu.stage0a[3] = static_cast<std::int32_t>(TextureOp::kSelectArg1);
        fu.stage0b[0] = static_cast<std::int32_t>(desc.stages[0].alphaArg1);
        if (keyColor.has_value()) {
            fu.stage0b[2] = 1;
            argbToFloats(*keyColor, fu.key0);
        }
        fu.misc[0] = static_cast<std::int32_t>(CompareFunction::kAlways);
        pass.commands.push_back(command);
    }

    // Encodes a pass: one copy of its staging data, then one render pass of its draws.
    void Encode(SDL_GPUCommandBuffer *commands, Pass &pass) {
        if (pass.commands.empty()) {
            return;
        }
        const auto bytes = static_cast<std::uint32_t>(pass.staging.size());
        if (!EnsureStream(bytes)) {
            pass.commands.clear();
            pass.staging.clear();
            return;
        }
        void *mapped = SDL_MapGPUTransferBuffer(device_, streamUpload_, true);
        if (mapped != nullptr) {
            std::memcpy(mapped, pass.staging.data(), bytes);
            SDL_UnmapGPUTransferBuffer(device_, streamUpload_);
            SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
            SDL_GPUTransferBufferLocation from{};
            from.transfer_buffer = streamUpload_;
            SDL_GPUBufferRegion to{};
            to.buffer = stream_;
            to.size = bytes;
            SDL_UploadToGPUBuffer(copy, &from, &to, true);
            SDL_EndGPUCopyPass(copy);
        }

        SDL_GPUColorTargetInfo color{};
        color.texture = pass.color;
        color.load_op = SDL_GPU_LOADOP_LOAD;
        color.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPUDepthStencilTargetInfo depth{};
        depth.texture = pass.depth;
        depth.load_op = SDL_GPU_LOADOP_LOAD;
        depth.store_op = SDL_GPU_STOREOP_STORE;
        depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        SDL_GPURenderPass *render =
            SDL_BeginGPURenderPass(commands, pass.color != nullptr ? &color : nullptr,
                                   pass.color != nullptr ? 1 : 0,
                                   pass.depth != nullptr ? &depth : nullptr);
        // The SDL viewport is the whole target (vertices are target pixels); the Viewport's
        // rectangle is the scissor below and its depth range is applied here, per draw.
        SDL_GPUViewport viewport{};
        viewport.w = static_cast<float>(pass.width);
        viewport.h = static_cast<float>(pass.height);
        viewport.min_depth = 0.0f;
        viewport.max_depth = 1.0f;
        SDL_SetGPUViewport(render, &viewport);
        SDL_GPUGraphicsPipeline *bound = nullptr;
        for (const Command &c : pass.commands) {
            if (c.minDepth != viewport.min_depth || c.maxDepth != viewport.max_depth) {
                viewport.min_depth = c.minDepth;
                viewport.max_depth = c.maxDepth;
                SDL_SetGPUViewport(render, &viewport);
            }
            if (c.pipeline != bound) {
                SDL_BindGPUGraphicsPipeline(render, c.pipeline);
                bound = c.pipeline;
            }
            SDL_SetGPUScissor(render, &c.scissor);
            SDL_GPUBufferBinding vertexBindings[2]{};
            vertexBindings[0].buffer = stream_;
            vertexBindings[0].offset = c.vertexOffset;
            vertexBindings[1].buffer = defaults_;
            SDL_BindGPUVertexBuffers(render, 0, vertexBindings, 2);
            SDL_GPUTextureSamplerBinding samplers[kTextureStages]{};
            for (std::size_t s = 0; s < kTextureStages; ++s) {
                samplers[s].texture = c.textures[s];
                samplers[s].sampler = c.samplers[s];
            }
            SDL_BindGPUFragmentSamplers(render, 0, samplers, kTextureStages);
            SDL_PushGPUVertexUniformData(commands, 0, &c.vertexUniforms, sizeof c.vertexUniforms);
            SDL_PushGPUFragmentUniformData(commands, 0, &c.fragmentUniforms,
                                           sizeof c.fragmentUniforms);
            if (c.indexed) {
                SDL_GPUBufferBinding indexBinding{};
                indexBinding.buffer = stream_;
                indexBinding.offset = c.indexOffset;
                SDL_BindGPUIndexBuffer(render, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
                SDL_DrawGPUIndexedPrimitives(render, c.indexCount, 1, 0, 0, 0);
            } else {
                SDL_DrawGPUPrimitives(render, c.vertexCount, 1, 0, 0);
            }
        }
        SDL_EndGPURenderPass(render);
        pass.commands.clear();
        pass.staging.clear();
    }

    bool EnsureStream(std::uint32_t bytes) {
        if (bytes <= streamSize_ && stream_ != nullptr) {
            return true;
        }
        std::uint32_t size = 64u * 1024u;
        while (size < bytes) {
            size *= 2;
        }
        SDL_ReleaseGPUBuffer(device_, stream_);
        SDL_ReleaseGPUTransferBuffer(device_, streamUpload_);
        SDL_GPUBufferCreateInfo info{};
        info.usage = SDL_GPU_BUFFERUSAGE_VERTEX | SDL_GPU_BUFFERUSAGE_INDEX;
        info.size = size;
        stream_ = SDL_CreateGPUBuffer(device_, &info);
        SDL_GPUTransferBufferCreateInfo upload{};
        upload.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        upload.size = size;
        streamUpload_ = SDL_CreateGPUTransferBuffer(device_, &upload);
        streamSize_ = stream_ != nullptr && streamUpload_ != nullptr ? size : 0;
        return streamSize_ != 0;
    }

    void FlushScene() {
        if (inScene_) {
            Encode(sceneCommands_, scene_);
        }
    }

    // --- blits and fills --------------------------------------------------------------------

    static bool Overlap(const Rect &a, const Rect &b) {
        return a.x < b.x + static_cast<std::int32_t>(b.width) &&
               b.x < a.x + static_cast<std::int32_t>(a.width) &&
               a.y < b.y + static_cast<std::int32_t>(b.height) &&
               b.y < a.y + static_cast<std::int32_t>(a.height);
    }

    // Indexed to indexed: indices are copied in memory, keyed by index, then re-expanded.
    Status BlitIndexed(const TextureRec &src, const Rect &from, TextureRec &dst, const Rect &to,
                       std::optional<std::uint32_t> key) {
        const std::vector<std::uint8_t> source = src.indices[0];
        std::vector<std::uint8_t> &target = dst.indices[0];
        for (std::uint32_t row = 0; row < to.height; ++row) {
            const std::uint32_t sy = from.y + (row * from.height + from.height / 2) / to.height;
            for (std::uint32_t col = 0; col < to.width; ++col) {
                const std::uint32_t sx = from.x + (col * from.width + from.width / 2) / to.width;
                const std::uint8_t index = source[std::size_t{sy} * src.desc.width + sx];
                if (key.has_value() && index == *key) {
                    continue;
                }
                target[std::size_t{to.y + row} * dst.desc.width + to.x + col] = index;
            }
        }
        return Expand(dst, 0, to);
    }

    // Anything into a direct-colour texture: a point-sampled quad through the source.
    Status BlitGpu(TextureRec &src, const Rect &from, TextureRec &dst, const Rect &to,
                   std::optional<std::uint32_t> key) {
        PipelineDesc desc = quadPipeline(TextureArg::kTexture);
        SDL_GPUGraphicsPipeline *pipeline = InternalPipeline(desc, true, true);
        if (pipeline == nullptr) {
            return Error::kOutOfResources;
        }
        SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device_);
        if (commands == nullptr) {
            return Error::kOutOfResources;
        }
        SDL_GPUTexture *source = src.gpu;
        SDL_GPUTexture *temporary = nullptr;
        float u0 = static_cast<float>(from.x) / static_cast<float>(src.desc.width);
        float v0 = static_cast<float>(from.y) / static_cast<float>(src.desc.height);
        float u1 = static_cast<float>(from.x + static_cast<std::int32_t>(from.width)) /
                   static_cast<float>(src.desc.width);
        float v1 = static_cast<float>(from.y + static_cast<std::int32_t>(from.height)) /
                   static_cast<float>(src.desc.height);
        if (&src == &dst) {
            // Sampling the texture being drawn into is a hazard; copy the source out first.
            SDL_GPUTextureCreateInfo info{};
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = kColorFormat;
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = from.width;
            info.height = from.height;
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            temporary = SDL_CreateGPUTexture(device_, &info);
            if (temporary == nullptr) {
                SDL_CancelGPUCommandBuffer(commands);
                return Error::kOutOfResources;
            }
            SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
            SDL_GPUTextureLocation a{};
            a.texture = src.gpu;
            a.x = static_cast<Uint32>(from.x);
            a.y = static_cast<Uint32>(from.y);
            SDL_GPUTextureLocation b{};
            b.texture = temporary;
            SDL_CopyGPUTextureToTexture(copy, &a, &b, from.width, from.height, 1, false);
            SDL_EndGPUCopyPass(copy);
            source = temporary;
            u0 = 0.0f;
            v0 = 0.0f;
            u1 = 1.0f;
            v1 = 1.0f;
        }
        Pass pass;
        pass.color = dst.gpu;
        pass.width = dst.desc.width;
        pass.height = dst.desc.height;
        std::optional<std::uint32_t> keyColor;
        if (key.has_value()) {
            keyColor = KeyColor(src, *key);
        }
        RecordQuad(pass, pipeline,
                   SDL_Rect{to.x, to.y, static_cast<int>(to.width), static_cast<int>(to.height)},
                   0.0f, 0xFFFFFFFFu, source, u0, v0, u1, v1, keyColor);
        Encode(commands, pass);
        SDL_SubmitGPUCommandBuffer(commands);
        if (temporary != nullptr) {
            SDL_ReleaseGPUTexture(device_, temporary);
        }
        return {};
    }

    Status FillDepth(TextureRec &rec, const Rect &area, float depth) {
        PipelineDesc desc = quadPipeline(TextureArg::kDiffuse);
        desc.depthTarget = true;
        desc.depthTest = true;
        desc.depthWrite = true;
        desc.depthFunction = CompareFunction::kAlways;
        SDL_GPUGraphicsPipeline *pipeline = InternalPipeline(desc, false, false);
        SDL_GPUCommandBuffer *commands =
            pipeline != nullptr ? SDL_AcquireGPUCommandBuffer(device_) : nullptr;
        if (commands == nullptr) {
            return Error::kOutOfResources;
        }
        Pass pass;
        pass.depth = rec.gpu;
        pass.width = rec.desc.width;
        pass.height = rec.desc.height;
        RecordQuad(pass, pipeline,
                   SDL_Rect{area.x, area.y, static_cast<int>(area.width),
                            static_cast<int>(area.height)},
                   depth, 0xFFFFFFFFu, nullptr, 0.0f, 0.0f, 0.0f, 0.0f, std::nullopt);
        Encode(commands, pass);
        SDL_SubmitGPUCommandBuffer(commands);
        return {};
    }

    // --- present ----------------------------------------------------------------------------

    std::uint32_t EffectiveFrameRate() {
        explicitCap_ = false;
        auto cap = window_->GetFrameCap();
        if (cap.Ok() && cap.Value().maxFramesPerSecond != 0) {
            explicitCap_ = true;
            return cap.Value().maxFramesPerSecond;
        }
        const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(sdlWindow_));
        if (mode != nullptr && mode->refresh_rate > 0.0f) {
            return static_cast<std::uint32_t>(std::lround(mode->refresh_rate));
        }
        return kDefaultRefreshHz;
    }

    // At the display's rate, vsync paces; with an explicit cap, mailbox (or vsync) presents and
    // Pace() waits.
    SDL_GPUPresentMode ChoosePresentMode() {
        if (explicitCap_ &&
            SDL_WindowSupportsGPUPresentMode(device_, sdlWindow_, SDL_GPU_PRESENTMODE_MAILBOX)) {
            return SDL_GPU_PRESENTMODE_MAILBOX;
        }
        return SDL_GPU_PRESENTMODE_VSYNC;
    }

    // `acquired` is whether the window gave a swapchain image; a minimised or occluded window
    // does not, and then nothing was shown and nothing waited.
    bool ShowBackBuffer(bool &acquired) {
        acquired = false;
        SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device_);
        if (commands == nullptr) {
            return false;
        }
        SDL_GPUTexture *swapchain = nullptr;
        Uint32 width = 0;
        Uint32 height = 0;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(commands, sdlWindow_, &swapchain, &width,
                                                   &height)) {
            SDL_CancelGPUCommandBuffer(commands);
            return false;
        }
        acquired = swapchain != nullptr;
        const TextureRec *back = FindTexture(backBuffer_);
        if (swapchain != nullptr && back != nullptr && width != 0 && height != 0) {
            const float bw = static_cast<float>(back->desc.width);
            const float bh = static_cast<float>(back->desc.height);
            const float scale =
                (std::min)(static_cast<float>(width) / bw, static_cast<float>(height) / bh);
            const auto dw = static_cast<Uint32>(std::lround(bw * scale));
            const auto dh = static_cast<Uint32>(std::lround(bh * scale));
            SDL_GPUBlitInfo blit{};
            blit.source.texture = back->gpu;
            blit.source.w = back->desc.width;
            blit.source.h = back->desc.height;
            blit.destination.texture = swapchain;
            blit.destination.x = (width - dw) / 2;
            blit.destination.y = (height - dh) / 2;
            blit.destination.w = dw;
            blit.destination.h = dh;
            blit.load_op = SDL_GPU_LOADOP_CLEAR;
            blit.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
            blit.filter = std::floor(scale) == scale ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
            SDL_BlitGPUTexture(commands, &blit);
        }
        SDL_SubmitGPUCommandBuffer(commands);
        return true;
    }

    // Waits until one frame period after the previous deadline: sleep, then spin. A deadline
    // missed by more than a frame starts the count again rather than rushing to catch up.
    void Pace(std::uint32_t fps) {
        if (fps == 0) {
            fps = kDefaultRefreshHz; // a display that reports a rate that rounds to 0
        }
        using Clock = std::chrono::steady_clock;
        const auto period = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(1.0 / static_cast<double>(fps)));
        const Clock::time_point now = Clock::now();
        if (!paced_ || now > deadline_ + period) {
            paced_ = true;
            deadline_ = now;
            return;
        }
        deadline_ += period;
        const auto margin = std::chrono::milliseconds(2);
        if (deadline_ - now > margin) {
            std::this_thread::sleep_for(deadline_ - now - margin);
        }
        while (Clock::now() < deadline_) {
            std::this_thread::yield();
        }
    }

    SDL_GPUDevice *device_ = nullptr;
    Window *window_ = nullptr;
    SDL_Window *sdlWindow_ = nullptr;
    bool claimed_ = false;
    SDL_GPUPresentMode presentMode_ = SDL_GPU_PRESENTMODE_VSYNC;
    bool explicitCap_ = false;
    bool paced_ = false;
    std::chrono::steady_clock::time_point deadline_{};

    SDL_GPUShader *vertexShader_ = nullptr;
    SDL_GPUShader *fragmentShader_ = nullptr;
    SDL_GPUBuffer *defaults_ = nullptr;
    SDL_GPUTexture *white_ = nullptr;
    SDL_GPUBuffer *stream_ = nullptr;
    SDL_GPUTransferBuffer *streamUpload_ = nullptr;
    std::uint32_t streamSize_ = 0;

    std::uint32_t nextHandle_ = 1; // never reused, so a handle from before Detach() stays dead
    std::unordered_map<std::uint32_t, BufferRec> buffers_;
    std::unordered_map<std::uint32_t, TextureRec> textures_;
    std::unordered_map<std::uint32_t, PaletteRec> palettes_;
    std::unordered_map<std::uint32_t, PipelineRec> pipelines_;
    std::map<std::string, std::uint32_t> pipelineIds_;
    std::map<std::string, SDL_GPUGraphicsPipeline *> internalPipelines_;
    std::unordered_map<std::uint32_t, SDL_GPUSampler *> samplers_;
    TextureId backBuffer_;

    bool inScene_ = false;
    SDL_GPUCommandBuffer *sceneCommands_ = nullptr;
    RenderTargets sceneTargets_;
    PixelFormat sceneFormat_ = PixelFormat::kRgb565;
    Pass scene_;
    SDL_Rect viewport_{};
    float viewportMinDepth_ = 0.0f;
    float viewportMaxDepth_ = 1.0f;
};

} // namespace

std::unique_ptr<Gpu> makeGpu() { return std::make_unique<GpuSdl>(); }

} // namespace sfcop::platform
