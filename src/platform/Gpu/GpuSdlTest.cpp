// The SDL GPU backend through the Gpu interface, rendering offscreen and reading the pixels back.
// The window is hidden (SFCOP_WINDOW_HIDDEN=1), so there is no swapchain; the device is real. It
// uses the platform's own video driver, because SDL's Metal device needs one that makes Metal
// views (the dummy driver does not). Every test skips with a message where no window or no GPU
// device can be had.
#include "platform/Gpu/Gpu.h"
#include "platform/Window/Window.h"
#include "platform/Window/WindowMock.h"

#include <SDL3/SDL.h>
#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace sfcop::platform {

// Defined in GpuSdl.cpp, declared there too and not in a header, because every header under
// src/platform/ is a contract.
namespace gpusdl {
Error noDeviceError(bool driverTakesCarriedFormat, bool driverTakesOtherFormat);
bool presentNeedsPacing(bool windowClaimed, bool explicitCap, bool swapchainAcquired);
SDL_GPUFilter presentFilter(TextureFilter filter, std::uint32_t frameW, std::uint32_t frameH,
                            std::uint32_t placeW, std::uint32_t placeH);
std::uint32_t presentPrescale(TextureFilter filter, std::uint32_t frameW, std::uint32_t frameH,
                              std::uint32_t placeW, std::uint32_t placeH);
Status scaleFrameInto(Gpu &gpu, TextureId frame, TextureId target, Rect place);
} // namespace gpusdl

namespace {

bool envIsOne(const char *name) {
    const char *value = std::getenv(name);
    return value != nullptr && std::strcmp(value, "1") == 0;
}

constexpr std::uint32_t kGreen = 0xFF00FF00u;
constexpr std::uint32_t kNavy = 0xFF000080u;
constexpr std::uint32_t kRed = 0xFFFF0000u;
constexpr std::uint32_t kBlue = 0xFF0000FFu;

struct TestVertex {
    float x, y, z, rhw;
    std::uint32_t diffuse;
    std::uint32_t specular;
    float u, v;
};

TestVertex at(float x, float y, std::uint32_t color, float z = 0.5f, float u = 0.0f,
              float v = 0.0f) {
    return TestVertex{x, y, z, 1.0f, color, 0xFF000000u, u, v};
}

// A hidden window and a Gpu attached to it, or `ready` false with the reason.
struct Rig {
    std::unique_ptr<Window> window;
    std::unique_ptr<Gpu> gpu;
    Status attached;
    bool ready = false;

    Rig() {
        SDL_setenv_unsafe("SFCOP_WINDOW_HIDDEN", "1", 1);
        window = makeWindow();
        gpu = makeGpu();
        WindowDesc desc;
        desc.title = "TestCaptain";
        attached = window->Create(desc);
        if (!attached.Ok()) {
            return; // no display: kOutOfResources
        }
        attached = gpu->Attach(*window);
        ready = attached.Ok();
        if (!ready) {
            REQUIRE((attached.Code() == Error::kIoError || attached.Code() == Error::kUnsupported));
        }
    }
    ~Rig() {
        (void)gpu->Detach();
        (void)window->Destroy();
    }
};

// A machine with no GPU device skips the test, loudly: the line goes to stderr so that it shows
// in a passing run. With SFCOP_REQUIRE_GPU=1 (a runner that has a GPU) the test fails instead.
#define REQUIRE_GPU(rig)                                                                         \
    if (!(rig).ready) {                                                                          \
        const std::string why = std::string(errorName((rig).attached.Code())) + ": " +          \
                                std::string(SDL_GetError());                                     \
        if (envIsOne("SFCOP_REQUIRE_GPU")) {                                                     \
            FAIL("SFCOP_REQUIRE_GPU=1 and no SDL GPU device (" << why << ")");                  \
        }                                                                                        \
        std::fprintf(stderr, "skipped: no GPU device (%s)\n", why.c_str());                      \
        MESSAGE("skipped: no GPU device (" << why << ")");                                       \
        return;                                                                                  \
    }

TextureId makeTarget(Gpu &gpu, std::uint32_t width, std::uint32_t height,
                     PixelFormat format = PixelFormat::kArgb8888) {
    TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.renderTarget = true;
    auto made = gpu.CreateTexture(desc);
    REQUIRE(made.Ok());
    return made.Value();
}

BufferId makeVertices(Gpu &gpu, const std::vector<TestVertex> &vertices) {
    const auto bytes = static_cast<std::uint32_t>(vertices.size() * sizeof(TestVertex));
    auto made = gpu.CreateBuffer(BufferKind::kVertex, bytes);
    REQUIRE(made.Ok());
    REQUIRE(gpu.WriteBuffer(made.Value(), 0, vertices.data(), bytes).Ok());
    return made.Value();
}

PipelineId makePipeline(Gpu &gpu, const PipelineDesc &desc) {
    auto made = gpu.CreatePipeline(desc);
    REQUIRE(made.Ok());
    return made.Value();
}

PipelineDesc argbPipeline() {
    PipelineDesc desc;
    desc.colorFormat = PixelFormat::kArgb8888;
    return desc;
}

std::vector<std::uint32_t> readArgb(Gpu &gpu, TextureId texture, std::uint32_t width,
                                    std::uint32_t height) {
    std::vector<std::uint32_t> pixels(std::size_t{width} * height, 0xDEADBEEFu);
    REQUIRE(gpu.ReadTexture(texture, 0, Rect{}, pixels.data(), width * 4).Ok());
    return pixels;
}

// One scene: clear the target to `clearColor`, draw `vertexCount` vertices, end.
void drawScene(Gpu &gpu, TextureId target, PipelineId pipeline, BufferId vertices,
               std::uint32_t vertexCount, std::uint32_t clearColor,
               const DrawState &state = DrawState{}) {
    REQUIRE(gpu.BeginScene(RenderTargets{target, TextureId{}}).Ok());
    ClearDesc clear;
    clear.colorValue = clearColor;
    REQUIRE(gpu.Clear(clear).Ok());
    DrawCall draw;
    draw.pipeline = pipeline;
    draw.vertices = vertices;
    draw.vertexCount = vertexCount;
    draw.state = state;
    REQUIRE(gpu.Draw(draw).Ok());
    REQUIRE(gpu.EndScene().Ok());
}

// The triangle (8,8), (56,8), (8,56), clockwise on screen as Direct3D's default culling keeps.
std::vector<TestVertex> greenTriangle() {
    return {at(8, 8, kGreen), at(56, 8, kGreen), at(8, 56, kGreen)};
}

// A palettised 4x4 texture: index 0 (magenta, the key), 1 (red), 2 (blue), rows of
//   0 1 1 0 / 1 2 2 1 / 1 2 2 1 / 0 1 1 0.
struct Palettised {
    TextureId texture;
    PaletteId palette;
};

Palettised makePalettised(Gpu &gpu) {
    std::vector<PaletteEntry> entries(kPaletteSize);
    entries[0] = PaletteEntry{255, 0, 255, 255};
    entries[1] = PaletteEntry{255, 0, 0, 255};
    entries[2] = PaletteEntry{0, 0, 255, 255};
    auto palette = gpu.CreatePalette(entries.data());
    REQUIRE(palette.Ok());
    TextureDesc desc;
    desc.width = 4;
    desc.height = 4;
    desc.format = PixelFormat::kIndexed8;
    auto texture = gpu.CreateTexture(desc);
    REQUIRE(texture.Ok());
    const std::uint8_t indices[16] = {0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1, 0, 1, 1, 0};
    REQUIRE(gpu.WriteTexture(texture.Value(), 0, Rect{}, indices, 4).Ok());
    REQUIRE(gpu.AttachPalette(texture.Value(), palette.Value()).Ok());
    REQUIRE(gpu.SetColorKey(texture.Value(), 0u).Ok());
    return Palettised{texture.Value(), palette.Value()};
}

// What the palettised texture leaves on green where index 0 is keyed out.
const std::vector<std::uint32_t> kKeyedOnGreen = {
    kGreen, kRed,  kRed,  kGreen, kRed,   kBlue, kBlue, kRed,
    kRed,   kBlue, kBlue, kRed,   kGreen, kRed,  kRed,  kGreen,
};

} // namespace

TEST_CASE("a triangle renders to an offscreen target and reads back") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    const TextureId target = makeTarget(gpu, 64, 64);
    const BufferId vertices = makeVertices(gpu, greenTriangle());
    drawScene(gpu, target, makePipeline(gpu, argbPipeline()), vertices, 3, kNavy);

    const std::vector<std::uint32_t> pixels = readArgb(gpu, target, 64, 64);
    const auto pixel = [&pixels](int x, int y) { return pixels[std::size_t(y) * 64 + x]; };
    CHECK(pixel(10, 10) == 0xFF00FF00u); // inside
    CHECK(pixel(20, 30) == 0xFF00FF00u);
    CHECK(pixel(40, 12) == 0xFF00FF00u);
    CHECK(pixel(4, 4) == 0xFF000080u); // outside
    CHECK(pixel(50, 50) == 0xFF000080u);
    CHECK(pixel(62, 2) == 0xFF000080u);
}

TEST_CASE("Direct3D's default culling discards a counter-clockwise triangle") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    const TextureId target = makeTarget(gpu, 64, 64);
    const BufferId vertices =
        makeVertices(gpu, {at(8, 8, kGreen), at(8, 56, kGreen), at(56, 8, kGreen)});
    drawScene(gpu, target, makePipeline(gpu, argbPipeline()), vertices, 3, kNavy);
    CHECK(readArgb(gpu, target, 64, 64)[10 * 64 + 10] == kNavy);

    PipelineDesc noCull = argbPipeline();
    noCull.cull = CullMode::kNone;
    drawScene(gpu, target, makePipeline(gpu, noCull), vertices, 3, kNavy);
    CHECK(readArgb(gpu, target, 64, 64)[10 * 64 + 10] == kGreen);
}

TEST_CASE("a palettised texture with a colour key blits and draws without its keyed index") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    const Palettised source = makePalettised(gpu);

    std::uint8_t indices[16] = {};
    REQUIRE(gpu.ReadTexture(source.texture, 0, Rect{}, indices, 4).Ok());
    CHECK(indices[0] == 0);
    CHECK(indices[5] == 2);

    const TextureId blitTarget = makeTarget(gpu, 4, 4);
    REQUIRE(gpu.Fill(blitTarget, Rect{}, kGreen).Ok());
    BlitDesc blit;
    blit.source = source.texture;
    blit.destination = blitTarget;
    blit.sourceColorKey = true;
    REQUIRE(gpu.Blit(blit).Ok());
    CHECK(readArgb(gpu, blitTarget, 4, 4) == kKeyedOnGreen);

    PipelineDesc keyed = argbPipeline();
    keyed.colorKey = true;
    keyed.cull = CullMode::kNone;
    keyed.stages[0].colorOp = TextureOp::kSelectArg1;
    keyed.stages[0].colorArg1 = TextureArg::kTexture;
    const std::vector<TestVertex> quad = {
        at(-0.5f, -0.5f, 0xFFFFFFFFu, 0.5f, 0, 0), at(3.5f, -0.5f, 0xFFFFFFFFu, 0.5f, 1, 0),
        at(-0.5f, 3.5f, 0xFFFFFFFFu, 0.5f, 0, 1),  at(3.5f, -0.5f, 0xFFFFFFFFu, 0.5f, 1, 0),
        at(3.5f, 3.5f, 0xFFFFFFFFu, 0.5f, 1, 1),   at(-0.5f, 3.5f, 0xFFFFFFFFu, 0.5f, 0, 1),
    };
    const TextureId drawTarget = makeTarget(gpu, 4, 4);
    DrawState state;
    state.textures[0] = source.texture;
    drawScene(gpu, drawTarget, makePipeline(gpu, keyed), makeVertices(gpu, quad), 6, kGreen,
              state);
    CHECK(readArgb(gpu, drawTarget, 4, 4) == kKeyedOnGreen);
}

TEST_CASE("a palette change shows in the blits after it") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    const Palettised source = makePalettised(gpu);
    const PaletteEntry green{0, 255, 0, 255};
    REQUIRE(gpu.WritePalette(source.palette, 2, 1, &green).Ok());
    const TextureId target = makeTarget(gpu, 4, 4);
    BlitDesc blit;
    blit.source = source.texture;
    blit.destination = target;
    REQUIRE(gpu.Blit(blit).Ok());
    const std::vector<std::uint32_t> pixels = readArgb(gpu, target, 4, 4);
    CHECK(pixels[0] == 0xFFFF00FFu); // index 0, magenta: not keyed without sourceColorKey
    CHECK(pixels[1] == kRed);
    CHECK(pixels[5] == kGreen); // index 2, now green
}

TEST_CASE("an indexed texture without a palette reads as black") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    const Palettised source = makePalettised(gpu);
    REQUIRE(gpu.DestroyPalette(source.palette).Ok());
    const TextureId target = makeTarget(gpu, 4, 4);
    BlitDesc blit;
    blit.source = source.texture;
    blit.destination = target;
    REQUIRE(gpu.Blit(blit).Ok());
    CHECK(readArgb(gpu, target, 4, 4)[5] == 0xFF000000u);
}

TEST_CASE("the same pipeline description returns the same pipeline handle") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    const PipelineId first = makePipeline(gpu, argbPipeline());
    const PipelineId again = makePipeline(gpu, argbPipeline());
    CHECK(first == again);
    PipelineDesc blended = argbPipeline();
    blended.blend = true;
    blended.sourceBlend = BlendFactor::kSourceAlpha;
    blended.destinationBlend = BlendFactor::kInverseSourceAlpha;
    const PipelineId other = makePipeline(gpu, blended);
    CHECK(other != first);
    // Each create is matched by a destroy; the handle dies with the last.
    CHECK(gpu.DestroyPipeline(first).Ok());
    CHECK(gpu.DestroyPipeline(again).Ok());
    CHECK(gpu.DestroyPipeline(first).Code() == Error::kInvalidHandle);
}

TEST_CASE("16-bit and 32-bit pixels read back as written") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    TextureDesc desc;
    desc.width = 2;
    desc.height = 2;

    desc.format = PixelFormat::kRgb565;
    TextureId rgb565 = gpu.CreateTexture(desc).Value();
    const std::uint16_t in565[4] = {0xF800, 0x07E0, 0x001F, 0x8410};
    REQUIRE(gpu.WriteTexture(rgb565, 0, Rect{}, in565, 4).Ok());
    std::uint16_t out565[4] = {};
    REQUIRE(gpu.ReadTexture(rgb565, 0, Rect{}, out565, 4).Ok());
    CHECK(out565[0] == 0xF800);
    CHECK(out565[1] == 0x07E0);
    CHECK(out565[2] == 0x001F);
    CHECK(out565[3] == 0x8410);

    desc.format = PixelFormat::kArgb1555;
    TextureId argb1555 = gpu.CreateTexture(desc).Value();
    const std::uint16_t in1555[4] = {0xFC00, 0x03E0, 0x801F, 0x4210};
    REQUIRE(gpu.WriteTexture(argb1555, 0, Rect{}, in1555, 4).Ok());
    std::uint16_t out1555[4] = {};
    REQUIRE(gpu.ReadTexture(argb1555, 0, Rect{}, out1555, 4).Ok());
    CHECK(out1555[0] == 0xFC00);
    CHECK(out1555[1] == 0x03E0);
    CHECK(out1555[2] == 0x801F);
    CHECK(out1555[3] == 0x4210);

    desc.format = PixelFormat::kXrgb8888;
    TextureId xrgb = gpu.CreateTexture(desc).Value();
    const std::uint32_t inX[4] = {0x00123456u, 0xAB654321u, 0x00000000u, 0x00FFFFFFu};
    REQUIRE(gpu.WriteTexture(xrgb, 0, Rect{}, inX, 8).Ok());
    std::uint32_t outX[4] = {};
    REQUIRE(gpu.ReadTexture(xrgb, 0, Rect{}, outX, 8).Ok());
    CHECK(outX[0] == 0xFF123456u); // the unused byte reads as 0xFF
    CHECK(outX[1] == 0xFF654321u);
    CHECK(outX[2] == 0xFF000000u);
    CHECK(outX[3] == 0xFFFFFFFFu);

    // A region with a row pitch wider than the row.
    const std::uint16_t row[4] = {0x1234, 0xFFFF, 0x5678, 0xFFFF};
    REQUIRE(gpu.WriteTexture(rgb565, 0, Rect{0, 0, 1, 2}, row, 4).Ok());
    std::uint16_t column[2] = {};
    REQUIRE(gpu.ReadTexture(rgb565, 0, Rect{0, 0, 1, 2}, column, 2).Ok());
    CHECK(column[0] == 0x1234);
    CHECK(column[1] == 0x5678);
}

TEST_CASE("Fill writes a raw value and a stretched blit repeats source pixels") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    const TextureId source = makeTarget(gpu, 2, 2, PixelFormat::kRgb565);
    REQUIRE(gpu.Fill(source, Rect{}, 0x07E0).Ok());
    REQUIRE(gpu.Fill(source, Rect{1, 0, 1, 1}, 0xF800).Ok());
    std::uint16_t filled[4] = {};
    REQUIRE(gpu.ReadTexture(source, 0, Rect{}, filled, 4).Ok());
    CHECK(filled[0] == 0x07E0);
    CHECK(filled[1] == 0xF800);
    CHECK(filled[2] == 0x07E0);

    const TextureId target = makeTarget(gpu, 4, 4, PixelFormat::kRgb565);
    BlitDesc blit;
    blit.source = source;
    blit.destination = target;
    REQUIRE(gpu.Blit(blit).Ok());
    std::uint16_t out[16] = {};
    REQUIRE(gpu.ReadTexture(target, 0, Rect{}, out, 8).Ok());
    const std::uint16_t expected[16] = {0x07E0, 0x07E0, 0xF800, 0xF800, 0x07E0, 0x07E0,
                                        0xF800, 0xF800, 0x07E0, 0x07E0, 0x07E0, 0x07E0,
                                        0x07E0, 0x07E0, 0x07E0, 0x07E0};
    for (int i = 0; i < 16; ++i) {
        CHECK(out[i] == expected[i]);
    }
}

TEST_CASE("a blit within one texture copies between rectangles that do not overlap") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    const TextureId texture = makeTarget(gpu, 4, 2);
    REQUIRE(gpu.Fill(texture, Rect{}, kNavy).Ok());
    REQUIRE(gpu.Fill(texture, Rect{0, 0, 2, 2}, kRed).Ok());
    BlitDesc blit;
    blit.source = texture;
    blit.sourceRect = Rect{0, 0, 2, 2};
    blit.destination = texture;
    blit.destinationRect = Rect{2, 0, 2, 2};
    REQUIRE(gpu.Blit(blit).Ok());
    const std::vector<std::uint32_t> expected = {kRed, kRed, kRed, kRed,
                                                 kRed, kRed, kRed, kRed};
    CHECK(readArgb(gpu, texture, 4, 2) == expected);
    blit.destinationRect = Rect{1, 0, 2, 2};
    CHECK(gpu.Blit(blit).Code() == Error::kInvalidArgument);
}

TEST_CASE("the depth test keeps the nearer surface") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    const TextureId color = makeTarget(gpu, 16, 16);
    const TextureId depth = makeTarget(gpu, 16, 16, PixelFormat::kDepth16);
    PipelineDesc desc = argbPipeline();
    desc.depthTarget = true;
    desc.depthTest = true;
    desc.depthFunction = CompareFunction::kLess;
    const PipelineId pipeline = makePipeline(gpu, desc);
    const BufferId near = makeVertices(
        gpu, {at(-0.5f, -0.5f, kRed, 0.25f), at(31.5f, -0.5f, kRed, 0.25f),
              at(-0.5f, 31.5f, kRed, 0.25f)});
    const BufferId far = makeVertices(
        gpu, {at(-0.5f, -0.5f, kGreen, 0.75f), at(31.5f, -0.5f, kGreen, 0.75f),
              at(-0.5f, 31.5f, kGreen, 0.75f)});

    REQUIRE(gpu.BeginScene(RenderTargets{color, depth}).Ok());
    ClearDesc clear;
    clear.depth = true;
    clear.colorValue = kNavy;
    REQUIRE(gpu.Clear(clear).Ok());
    DrawCall draw;
    draw.pipeline = pipeline;
    draw.vertexCount = 3;
    draw.vertices = near;
    REQUIRE(gpu.Draw(draw).Ok());
    draw.vertices = far;
    REQUIRE(gpu.Draw(draw).Ok());
    REQUIRE(gpu.EndScene().Ok());
    CHECK(readArgb(gpu, color, 16, 16)[5 * 16 + 5] == kRed);

    // A depth target filled with 0 (nearest) lets nothing through a "less" test.
    REQUIRE(gpu.Fill(depth, Rect{}, 0).Ok());
    REQUIRE(gpu.BeginScene(RenderTargets{color, depth}).Ok());
    ClearDesc colorOnly;
    colorOnly.colorValue = kNavy;
    REQUIRE(gpu.Clear(colorOnly).Ok());
    REQUIRE(gpu.Draw(draw).Ok());
    REQUIRE(gpu.EndScene().Ok());
    CHECK(readArgb(gpu, color, 16, 16)[5 * 16 + 5] == kNavy);
}

TEST_CASE("a recorded draw keeps the vertices it saw") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    const TextureId target = makeTarget(gpu, 64, 64);
    const BufferId vertices = makeVertices(gpu, greenTriangle());
    REQUIRE(gpu.BeginScene(RenderTargets{target, TextureId{}}).Ok());
    ClearDesc clear;
    clear.colorValue = kNavy;
    REQUIRE(gpu.Clear(clear).Ok());
    DrawCall draw;
    draw.pipeline = makePipeline(gpu, argbPipeline());
    draw.vertices = vertices;
    draw.vertexCount = 3;
    REQUIRE(gpu.Draw(draw).Ok());
    const std::vector<TestVertex> red = {at(8, 8, kRed), at(56, 8, kRed), at(8, 56, kRed)};
    REQUIRE(gpu.WriteBuffer(vertices, 0, red.data(), sizeof(TestVertex) * 3).Ok());
    REQUIRE(gpu.EndScene().Ok());
    CHECK(readArgb(gpu, target, 64, 64)[10 * 64 + 10] == kGreen);
}

TEST_CASE("a texture destroyed after a draw that samples it does not change that draw") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    const TextureId texture = makeTarget(gpu, 1, 1);
    REQUIRE(gpu.Fill(texture, Rect{}, kRed).Ok());
    const TextureId target = makeTarget(gpu, 64, 64);
    REQUIRE(gpu.BeginScene(RenderTargets{target, TextureId{}}).Ok());
    DrawCall draw;
    draw.pipeline = makePipeline(gpu, argbPipeline()); // texture modulates diffuse
    draw.vertices = makeVertices(gpu, {at(8, 8, 0xFFFFFFFFu), at(56, 8, 0xFFFFFFFFu),
                                       at(8, 56, 0xFFFFFFFFu)});
    draw.vertexCount = 3;
    draw.state.textures[0] = texture;
    REQUIRE(gpu.Draw(draw).Ok());
    REQUIRE(gpu.DestroyTexture(texture).Ok());
    REQUIRE(gpu.EndScene().Ok());
    CHECK(readArgb(gpu, target, 64, 64)[10 * 64 + 10] == kRed);
}

TEST_CASE("fans and indexed triangles draw the same quad") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    const std::vector<TestVertex> corners = {at(-0.5f, -0.5f, kGreen), at(7.5f, -0.5f, kGreen),
                                             at(7.5f, 7.5f, kGreen), at(-0.5f, 7.5f, kGreen)};
    const BufferId vertices = makeVertices(gpu, corners);
    const TextureId target = makeTarget(gpu, 8, 8);
    const std::vector<std::uint32_t> allGreen(64, kGreen);

    PipelineDesc fan = argbPipeline();
    fan.primitive = PrimitiveType::kTriangleFan;
    drawScene(gpu, target, makePipeline(gpu, fan), vertices, 4, kNavy);
    CHECK(readArgb(gpu, target, 8, 8) == allGreen);

    const std::uint16_t list[6] = {0, 1, 2, 0, 2, 3};
    auto indices = gpu.CreateBuffer(BufferKind::kIndex, sizeof list);
    REQUIRE(indices.Ok());
    REQUIRE(gpu.WriteBuffer(indices.Value(), 0, list, sizeof list).Ok());
    REQUIRE(gpu.BeginScene(RenderTargets{target, TextureId{}}).Ok());
    ClearDesc clear;
    clear.colorValue = kNavy;
    REQUIRE(gpu.Clear(clear).Ok());
    DrawCall draw;
    draw.pipeline = makePipeline(gpu, argbPipeline());
    draw.vertices = vertices;
    draw.vertexCount = 4;
    draw.indices = indices.Value();
    draw.indexCount = 6;
    REQUIRE(gpu.Draw(draw).Ok());
    REQUIRE(gpu.EndScene().Ok());
    CHECK(readArgb(gpu, target, 8, 8) == allGreen);
}

TEST_CASE("the back buffer starts black, presents headless, and dies when reconfigured") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    CHECK(gpu.Present().Code() == Error::kWrongState); // no back buffer yet
    auto back = gpu.ConfigureBackBuffer(640, 480, PixelFormat::kRgb565);
    REQUIRE(back.Ok());
    std::uint16_t corner[1] = {0xFFFF};
    REQUIRE(gpu.ReadTexture(back.Value(), 0, Rect{0, 0, 1, 1}, corner, 2).Ok());
    CHECK(corner[0] == 0x0000);
    CHECK(gpu.Present().Ok());
    CHECK(gpu.DestroyTexture(back.Value()).Code() == Error::kWrongState);
    auto again = gpu.ConfigureBackBuffer(800, 600, PixelFormat::kRgb565);
    REQUIRE(again.Ok());
    CHECK(gpu.ReadTexture(back.Value(), 0, Rect{0, 0, 1, 1}, corner, 2).Code() ==
          Error::kInvalidHandle);
    CHECK(gpu.ConfigureBackBuffer(0, 480, PixelFormat::kRgb565).Code() == Error::kInvalidArgument);
    CHECK(gpu.ConfigureBackBuffer(640, 480, PixelFormat::kDepth16).Code() ==
          Error::kInvalidArgument);
    CHECK(gpu.ConfigureBackBuffer(8193, 480, PixelFormat::kRgb565).Code() ==
          Error::kInvalidArgument);
}

// The one test that reads a real clock: the frame cap is what it measures. CI under load sets
// SFCOP_SKIP_TIMING_TESTS=1.
TEST_CASE("the frame cap holds 60 frames a second over a 5 s headless run" *
          doctest::test_suite("timing") *
          doctest::skip(envIsOne("SFCOP_SKIP_TIMING_TESTS"))) {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    REQUIRE(rig.window->SetFrameCap(FrameCap{60}).Ok());
    auto back = gpu.ConfigureBackBuffer(640, 480, PixelFormat::kRgb565);
    REQUIRE(back.Ok());
    ClearDesc clear;
    clear.colorValue = kNavy;
    using Clock = std::chrono::steady_clock;
    const Clock::time_point end = Clock::now() + std::chrono::seconds(5);
    int frames = 0;
    while (Clock::now() < end) {
        REQUIRE(gpu.BeginScene(RenderTargets{back.Value(), TextureId{}}).Ok());
        REQUIRE(gpu.Clear(clear).Ok());
        REQUIRE(gpu.EndScene().Ok());
        REQUIRE(gpu.Present().Ok());
        ++frames;
    }
    MESSAGE("frames presented in 5 s with the cap at 60: " << frames);
    CHECK(frames >= 297);
    CHECK(frames <= 303);
}

TEST_CASE("caps list every format and the size limit") {
    Rig rig;
    REQUIRE_GPU(rig);
    auto caps = rig.gpu->GetCaps();
    REQUIRE(caps.Ok());
    CHECK(caps.Value().maxTextureSize == 8192);
    CHECK(caps.Value().textureFormats.size() == 6);
    CHECK(caps.Value().linearFiltering);
}

TEST_CASE("Attach refuses a foreign or closed window and a second attach") {
    Rig rig;
    REQUIRE_GPU(rig);
    CHECK(rig.gpu->Attach(*rig.window).Code() == Error::kWrongState);
    auto other = makeGpu();
    WindowMock mock;
    CHECK(other->Attach(mock).Code() == Error::kInvalidArgument);
    auto closed = makeWindow();
    CHECK(other->Attach(*closed).Code() == Error::kInvalidArgument);
    CHECK(other->Detach().Ok()); // detaching a detached Gpu succeeds
}

TEST_CASE("an unattached SDL Gpu refuses work with WrongState") {
    auto gpu = makeGpu();
    CHECK(gpu->GetCaps().Code() == Error::kWrongState);
    CHECK(gpu->ConfigureBackBuffer(640, 480, PixelFormat::kRgb565).Code() == Error::kWrongState);
    CHECK(gpu->CreateBuffer(BufferKind::kVertex, 32).Code() == Error::kWrongState);
    CHECK(gpu->CreateTexture(TextureDesc{}).Code() == Error::kWrongState);
    CHECK(gpu->CreatePalette(nullptr).Code() == Error::kWrongState);
    CHECK(gpu->CreatePipeline(PipelineDesc{}).Code() == Error::kWrongState);
    CHECK(gpu->BeginScene(RenderTargets{}).Code() == Error::kWrongState);
    CHECK(gpu->EndScene().Code() == Error::kWrongState);
    CHECK(gpu->Present().Code() == Error::kWrongState);
    CHECK(gpu->WriteBuffer(BufferId{1}, 0, nullptr, 0).Code() == Error::kInvalidHandle);
}

TEST_CASE("Detach kills every handle") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    const TextureId texture = makeTarget(gpu, 4, 4);
    const BufferId buffer = makeVertices(gpu, greenTriangle());
    REQUIRE(gpu.Detach().Ok());
    REQUIRE(gpu.Attach(*rig.window).Ok());
    std::uint32_t pixel = 0;
    CHECK(gpu.ReadTexture(texture, 0, Rect{0, 0, 1, 1}, &pixel, 4).Code() ==
          Error::kInvalidHandle);
    CHECK(gpu.DestroyBuffer(buffer).Code() == Error::kInvalidHandle);
}

TEST_CASE("buffer calls refuse bad sizes, ranges and handles") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    CHECK(gpu.CreateBuffer(BufferKind::kVertex, 0).Code() == Error::kInvalidArgument);
    CHECK(gpu.CreateBuffer(BufferKind::kIndex, 7).Code() == Error::kInvalidArgument);
    auto buffer = gpu.CreateBuffer(BufferKind::kVertex, 16);
    REQUIRE(buffer.Ok());
    const std::uint8_t bytes[16] = {};
    CHECK(gpu.WriteBuffer(buffer.Value(), 8, bytes, 9).Code() == Error::kInvalidArgument);
    CHECK(gpu.WriteBuffer(buffer.Value(), 0, bytes, 16).Ok());
    CHECK(gpu.WriteBuffer(BufferId{}, 0, bytes, 1).Code() == Error::kInvalidHandle);
    CHECK(gpu.DestroyBuffer(buffer.Value()).Ok());
    CHECK(gpu.DestroyBuffer(buffer.Value()).Code() == Error::kInvalidHandle);
}

TEST_CASE("texture calls refuse bad descriptions, regions and states") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    TextureDesc desc;
    desc.width = 4;
    desc.height = 4;
    desc.format = PixelFormat::kRgb565;
    desc.width = 0;
    CHECK(gpu.CreateTexture(desc).Code() == Error::kInvalidArgument);
    desc.width = 8193;
    CHECK(gpu.CreateTexture(desc).Code() == Error::kInvalidArgument);
    desc.width = 4;
    desc.mipLevels = 4; // 4x4 has levels 4, 2, 1
    CHECK(gpu.CreateTexture(desc).Code() == Error::kInvalidArgument);
    desc.mipLevels = 3;
    auto mipped = gpu.CreateTexture(desc);
    REQUIRE(mipped.Ok());
    desc.mipLevels = 1;
    desc.format = PixelFormat::kDepth16;
    CHECK(gpu.CreateTexture(desc).Code() == Error::kInvalidArgument);

    std::uint16_t pixels[16] = {};
    const TextureId texture = mipped.Value();
    CHECK(gpu.WriteTexture(texture, 3, Rect{}, pixels, 8).Code() == Error::kInvalidArgument);
    CHECK(gpu.WriteTexture(texture, 0, Rect{3, 3, 2, 1}, pixels, 8).Code() ==
          Error::kInvalidArgument);
    CHECK(gpu.WriteTexture(texture, 0, Rect{}, pixels, 7).Code() == Error::kInvalidArgument);
    CHECK(gpu.WriteTexture(texture, 2, Rect{}, pixels, 2).Ok()); // level 2 is 1x1
    CHECK(gpu.ReadTexture(texture, 0, Rect{-1, 0, 1, 1}, pixels, 8).Code() ==
          Error::kInvalidArgument);
    CHECK(gpu.WriteTexture(TextureId{}, 0, Rect{}, pixels, 8).Code() == Error::kInvalidHandle);

    const TextureId depth = makeTarget(gpu, 4, 4, PixelFormat::kDepth16);
    CHECK(gpu.WriteTexture(depth, 0, Rect{}, pixels, 8).Code() == Error::kInvalidArgument);
    CHECK(gpu.ReadTexture(depth, 0, Rect{}, pixels, 8).Code() == Error::kInvalidArgument);
    CHECK(gpu.SetColorKey(depth, 0u).Code() == Error::kInvalidArgument);
    CHECK(gpu.SetColorKey(texture, 0x10000u).Code() == Error::kInvalidArgument);
    CHECK(gpu.SetColorKey(texture, 0xF81Fu).Ok());
    CHECK(gpu.SetColorKey(texture, std::nullopt).Ok());

    const TextureId target = makeTarget(gpu, 4, 4);
    REQUIRE(gpu.BeginScene(RenderTargets{target, TextureId{}}).Ok());
    std::uint32_t argb[16] = {};
    CHECK(gpu.WriteTexture(target, 0, Rect{}, argb, 16).Code() == Error::kWrongState);
    CHECK(gpu.ReadTexture(target, 0, Rect{}, argb, 16).Code() == Error::kWrongState);
    CHECK(gpu.DestroyTexture(target).Code() == Error::kWrongState);
    REQUIRE(gpu.EndScene().Ok());
    CHECK(gpu.DestroyTexture(target).Ok());
    CHECK(gpu.DestroyTexture(target).Code() == Error::kInvalidHandle);
}

TEST_CASE("palette calls refuse bad ranges, handles and textures") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    auto palette = gpu.CreatePalette(nullptr);
    REQUIRE(palette.Ok());
    PaletteEntry entries[2] = {};
    CHECK(gpu.WritePalette(palette.Value(), 255, 2, entries).Code() == Error::kInvalidArgument);
    CHECK(gpu.WritePalette(palette.Value(), 254, 2, entries).Ok());
    CHECK(gpu.WritePalette(PaletteId{}, 0, 1, entries).Code() == Error::kInvalidHandle);
    const TextureId direct = makeTarget(gpu, 4, 4);
    CHECK(gpu.AttachPalette(direct, palette.Value()).Code() == Error::kInvalidArgument);
    CHECK(gpu.AttachPalette(TextureId{}, palette.Value()).Code() == Error::kInvalidHandle);
    CHECK(gpu.DestroyPalette(palette.Value()).Ok());
    CHECK(gpu.DestroyPalette(palette.Value()).Code() == Error::kInvalidHandle);
}

TEST_CASE("CreatePipeline refuses inconsistent descriptions") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    PipelineDesc indexed;
    indexed.colorFormat = PixelFormat::kIndexed8;
    CHECK(gpu.CreatePipeline(indexed).Code() == Error::kInvalidArgument);
    PipelineDesc depth;
    depth.colorFormat = PixelFormat::kDepth16;
    CHECK(gpu.CreatePipeline(depth).Code() == Error::kInvalidArgument);
    PipelineDesc gap;
    gap.stages[0].colorOp = TextureOp::kDisable;
    gap.stages[1].colorOp = TextureOp::kModulate;
    gap.stages[1].texCoordIndex = 0;
    CHECK(gpu.CreatePipeline(gap).Code() == Error::kInvalidArgument);
    PipelineDesc sets;
    sets.stages[0].texCoordIndex = 1; // the layout has one set
    CHECK(gpu.CreatePipeline(sets).Code() == Error::kInvalidArgument);
}

TEST_CASE("scene calls refuse bad targets, viewports and draws") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    TextureDesc plain;
    plain.width = 8;
    plain.height = 8;
    plain.format = PixelFormat::kArgb8888;
    const TextureId notTarget = gpu.CreateTexture(plain).Value();
    const TextureId target = makeTarget(gpu, 8, 8);
    const TextureId smallDepth = makeTarget(gpu, 4, 4, PixelFormat::kDepth16);
    const TextureId depth = makeTarget(gpu, 8, 8, PixelFormat::kDepth16);

    CHECK(gpu.BeginScene(RenderTargets{notTarget, TextureId{}}).Code() == Error::kInvalidArgument);
    CHECK(gpu.BeginScene(RenderTargets{target, smallDepth}).Code() == Error::kInvalidArgument);
    CHECK(gpu.BeginScene(RenderTargets{depth, TextureId{}}).Code() == Error::kInvalidArgument);
    CHECK(gpu.BeginScene(RenderTargets{TextureId{}, TextureId{}}).Code() == Error::kInvalidHandle);
    CHECK(gpu.SetViewport(Viewport{}).Code() == Error::kWrongState);
    CHECK(gpu.Clear(ClearDesc{}).Code() == Error::kWrongState);
    CHECK(gpu.Draw(DrawCall{}).Code() == Error::kWrongState);

    REQUIRE(gpu.BeginScene(RenderTargets{target, TextureId{}}).Ok());
    CHECK(gpu.BeginScene(RenderTargets{target, TextureId{}}).Code() == Error::kWrongState);
    CHECK(gpu.SetViewport(Viewport{4, 4, 8, 8, 0.0f, 1.0f}).Code() == Error::kInvalidArgument);
    CHECK(gpu.SetViewport(Viewport{0, 0, 0, 0, 0.8f, 0.2f}).Code() == Error::kInvalidArgument);
    CHECK(gpu.SetViewport(Viewport{2, 2, 4, 4, 0.0f, 1.0f}).Ok());
    ClearDesc depthClear;
    depthClear.depth = true;
    CHECK(gpu.Clear(depthClear).Code() == Error::kWrongState);

    const BufferId vertices = makeVertices(gpu, greenTriangle());
    DrawCall draw;
    draw.pipeline = makePipeline(gpu, argbPipeline());
    draw.vertices = vertices;
    draw.vertexCount = 4; // the buffer holds 3
    CHECK(gpu.Draw(draw).Code() == Error::kInvalidArgument);
    draw.vertexCount = 2; // not a whole triangle
    CHECK(gpu.Draw(draw).Code() == Error::kInvalidArgument);
    draw.vertexCount = 3;
    draw.state.textures[1] = target; // stage 1 is disabled
    CHECK(gpu.Draw(draw).Code() == Error::kInvalidArgument);
    draw.state.textures[1] = TextureId{};
    PipelineDesc rgb565;
    draw.pipeline = makePipeline(gpu, rgb565); // the target is ARGB8888
    CHECK(gpu.Draw(draw).Code() == Error::kWrongState);
    draw.pipeline = PipelineId{};
    CHECK(gpu.Draw(draw).Code() == Error::kInvalidHandle);
    CHECK(gpu.Blit(BlitDesc{}).Code() == Error::kWrongState);
    CHECK(gpu.Fill(target, Rect{}, 0).Code() == Error::kWrongState);
    CHECK(gpu.Present().Code() == Error::kWrongState);
    CHECK(gpu.ConfigureBackBuffer(64, 64, PixelFormat::kRgb565).Code() == Error::kWrongState);
    REQUIRE(gpu.EndScene().Ok());
    CHECK(gpu.EndScene().Code() == Error::kWrongState);
}

TEST_CASE("blits and fills refuse bad handles, formats and areas") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    const TextureId a565 = makeTarget(gpu, 4, 4, PixelFormat::kRgb565);
    const TextureId b8888 = makeTarget(gpu, 4, 4);
    const TextureId depth = makeTarget(gpu, 4, 4, PixelFormat::kDepth16);
    BlitDesc blit;
    blit.source = a565;
    blit.destination = b8888;
    CHECK(gpu.Blit(blit).Code() == Error::kInvalidArgument); // formats differ
    blit.destination = depth;
    CHECK(gpu.Blit(blit).Code() == Error::kInvalidArgument);
    blit.destination = TextureId{};
    CHECK(gpu.Blit(blit).Code() == Error::kInvalidHandle);
    blit.destination = a565;
    blit.sourceRect = Rect{2, 2, 4, 4};
    CHECK(gpu.Blit(blit).Code() == Error::kInvalidArgument);
    CHECK(gpu.Fill(a565, Rect{4, 0, 1, 1}, 0).Code() == Error::kInvalidArgument);
    CHECK(gpu.Fill(TextureId{}, Rect{}, 0).Code() == Error::kInvalidHandle);
    CHECK(gpu.Fill(depth, Rect{}, 0xFFFF).Ok());
}

TEST_CASE("a missing shader format is told from a device failure") {
    // The driver takes a format this build carries: the device failed, whatever the cause.
    CHECK(gpusdl::noDeviceError(true, false) == Error::kIoError);
    CHECK(gpusdl::noDeviceError(true, true) == Error::kIoError); // Metal: MSL carried, metallib not
    // The driver takes only formats this build lacks.
    CHECK(gpusdl::noDeviceError(false, true) == Error::kUnsupported);
    // The driver takes none we know of.
    CHECK(gpusdl::noDeviceError(false, false) == Error::kIoError);
}

TEST_CASE("Present paces unless vsync is pacing a window that gave a swapchain") {
    CHECK_FALSE(gpusdl::presentNeedsPacing(true, false, true)); // vsync paces
    CHECK(gpusdl::presentNeedsPacing(true, false, false));      // minimised or occluded
    CHECK(gpusdl::presentNeedsPacing(true, true, true));        // an explicit cap
    CHECK(gpusdl::presentNeedsPacing(false, false, false));     // headless
    CHECK(gpusdl::presentNeedsPacing(false, true, true));
}

TEST_CASE("the scaled frame is sampled linearly or by nearest pixel as the present filter says") {
    using gpusdl::presentFilter;
    // Linear (the default): linear at 1024x768 in 667x500 and 1440x1080, nearest at 1x and 2x.
    CHECK(presentFilter(TextureFilter::kLinear, 1024, 768, 667, 500) == SDL_GPU_FILTER_LINEAR);
    CHECK(presentFilter(TextureFilter::kLinear, 1024, 768, 1024, 768) == SDL_GPU_FILTER_NEAREST);
    CHECK(presentFilter(TextureFilter::kLinear, 1024, 768, 1440, 1080) == SDL_GPU_FILTER_LINEAR);
    CHECK(presentFilter(TextureFilter::kLinear, 640, 480, 1280, 960) == SDL_GPU_FILTER_NEAREST);
    // Whole on one axis only is not a whole-number scale.
    CHECK(presentFilter(TextureFilter::kLinear, 640, 480, 1280, 480) == SDL_GPU_FILTER_LINEAR);
    // Point: nearest at every scale.
    CHECK(presentFilter(TextureFilter::kPoint, 1024, 768, 667, 500) == SDL_GPU_FILTER_NEAREST);
    CHECK(presentFilter(TextureFilter::kPoint, 1024, 768, 1024, 768) == SDL_GPU_FILTER_NEAREST);
    CHECK(presentFilter(TextureFilter::kPoint, 1024, 768, 1440, 1080) == SDL_GPU_FILTER_NEAREST);
}

TEST_CASE("the SDL Gpu takes both present filters and refuses another value") {
    auto gpu = makeGpu();
    CHECK(gpu->SetPresentFilter(TextureFilter::kPoint).Ok());
    CHECK(gpu->SetPresentFilter(TextureFilter::kLinear).Ok());
    CHECK(gpu->SetPresentFilter(static_cast<TextureFilter>(7)).Code() == Error::kInvalidArgument);
}

TEST_CASE("a viewport limits drawing to its rectangle") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    const TextureId target = makeTarget(gpu, 64, 64);
    const BufferId vertices = makeVertices(
        gpu, {at(-0.5f, -0.5f, kGreen), at(127.5f, -0.5f, kGreen), at(-0.5f, 127.5f, kGreen)});
    PipelineDesc desc = argbPipeline();
    const PipelineId pipeline = makePipeline(gpu, desc);
    REQUIRE(gpu.BeginScene(RenderTargets{target, TextureId{}}).Ok());
    ClearDesc clear;
    clear.colorValue = kNavy;
    REQUIRE(gpu.Clear(clear).Ok());
    Viewport left;
    left.width = 32;
    left.height = 64;
    REQUIRE(gpu.SetViewport(left).Ok());
    DrawCall draw;
    draw.pipeline = pipeline;
    draw.vertices = vertices;
    draw.vertexCount = 3;
    REQUIRE(gpu.Draw(draw).Ok());
    REQUIRE(gpu.EndScene().Ok());
    const std::vector<std::uint32_t> pixels = readArgb(gpu, target, 64, 64);
    CHECK(pixels[10 * 64 + 10] == kGreen);
    CHECK(pixels[40 * 64 + 5] == kGreen);
    CHECK(pixels[10 * 64 + 31] == kGreen);
    CHECK(pixels[10 * 64 + 32] == kNavy);
    CHECK(pixels[10 * 64 + 50] == kNavy);
}

TEST_CASE("a viewport's depth range scales the depth the test sees") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    const TextureId color = makeTarget(gpu, 16, 16);
    const TextureId depth = makeTarget(gpu, 16, 16, PixelFormat::kDepth16);
    PipelineDesc desc = argbPipeline();
    desc.depthTarget = true;
    desc.depthTest = true;
    desc.depthFunction = CompareFunction::kLess;
    const PipelineId pipeline = makePipeline(gpu, desc);
    // z = 0.75 fails against a stored 0.4, but 0.75 in the range 0..0.5 is 0.375 and passes.
    const BufferId triangle = makeVertices(
        gpu, {at(-0.5f, -0.5f, kGreen, 0.75f), at(31.5f, -0.5f, kGreen, 0.75f),
              at(-0.5f, 31.5f, kGreen, 0.75f)});
    DrawCall draw;
    draw.pipeline = pipeline;
    draw.vertices = triangle;
    draw.vertexCount = 3;
    for (const float maxDepth : {1.0f, 0.5f}) {
        REQUIRE(gpu.Fill(depth, Rect{}, 26214).Ok()); // 0.4 of 65535
        REQUIRE(gpu.BeginScene(RenderTargets{color, depth}).Ok());
        ClearDesc colorOnly;
        colorOnly.colorValue = kNavy;
        REQUIRE(gpu.Clear(colorOnly).Ok());
        Viewport viewport;
        viewport.maxDepth = maxDepth;
        REQUIRE(gpu.SetViewport(viewport).Ok());
        REQUIRE(gpu.Draw(draw).Ok());
        REQUIRE(gpu.EndScene().Ok());
        CHECK(readArgb(gpu, color, 16, 16)[5 * 16 + 5] == (maxDepth == 1.0f ? kNavy : kGreen));
    }
}

TEST_CASE("additive blending sums the source and the destination") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    const TextureId target = makeTarget(gpu, 64, 64);
    PipelineDesc desc = argbPipeline();
    desc.blend = true;
    desc.sourceBlend = BlendFactor::kOne;
    desc.destinationBlend = BlendFactor::kOne;
    const BufferId vertices =
        makeVertices(gpu, {at(8, 8, 0xFF400000u), at(56, 8, 0xFF400000u), at(8, 56, 0xFF400000u)});
    drawScene(gpu, target, makePipeline(gpu, desc), vertices, 3, kNavy);
    const std::vector<std::uint32_t> pixels = readArgb(gpu, target, 64, 64);
    CHECK(pixels[10 * 64 + 10] == 0xFF400080u); // 0x40 red added to the navy's 0x80 blue
    CHECK(pixels[50 * 64 + 50] == kNavy);

    // Source alpha 0x00 over the navy, with the usual alpha blend, changes nothing.
    PipelineDesc over = argbPipeline();
    over.blend = true;
    over.sourceBlend = BlendFactor::kSourceAlpha;
    over.destinationBlend = BlendFactor::kInverseSourceAlpha;
    over.stages[0].alphaOp = TextureOp::kSelectArg1;
    over.stages[0].alphaArg1 = TextureArg::kDiffuse;
    const BufferId clear = makeVertices(
        gpu, {at(8, 8, 0x00FFFFFFu), at(56, 8, 0x00FFFFFFu), at(8, 56, 0x00FFFFFFu)});
    drawScene(gpu, target, makePipeline(gpu, over), clear, 3, kNavy);
    CHECK(readArgb(gpu, target, 64, 64)[10 * 64 + 10] == kNavy);
}

TEST_CASE("linear fog gives the fog colour beyond its end and none before its start") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    const TextureId target = makeTarget(gpu, 64, 64);
    PipelineDesc desc = argbPipeline();
    desc.fog = FogMode::kLinear;
    const PipelineId pipeline = makePipeline(gpu, desc);
    DrawState state;
    state.fogColor = 0x00FF0000u;
    state.fogStart = 0.25f;
    state.fogEnd = 0.5f;
    const BufferId far = makeVertices(
        gpu, {at(8, 8, kGreen, 0.75f), at(56, 8, kGreen, 0.75f), at(8, 56, kGreen, 0.75f)});
    drawScene(gpu, target, pipeline, far, 3, kNavy, state);
    CHECK(readArgb(gpu, target, 64, 64)[10 * 64 + 10] == kRed);
    const BufferId near = makeVertices(
        gpu, {at(8, 8, kGreen, 0.125f), at(56, 8, kGreen, 0.125f), at(8, 56, kGreen, 0.125f)});
    drawScene(gpu, target, pipeline, near, 3, kNavy, state);
    CHECK(readArgb(gpu, target, 64, 64)[10 * 64 + 10] == kGreen);
}

TEST_CASE("the alpha test discards a texel that fails the reference") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    TextureDesc textureDesc;
    textureDesc.width = 4;
    textureDesc.height = 4;
    textureDesc.format = PixelFormat::kArgb8888;
    auto texture = gpu.CreateTexture(textureDesc);
    REQUIRE(texture.Ok());
    std::uint32_t texels[16];
    for (int i = 0; i < 16; ++i) {
        texels[i] = (i % 4) < 2 ? 0x4000FF00u : 0xC000FF00u; // alpha 0x40 left, 0xC0 right
    }
    REQUIRE(gpu.WriteTexture(texture.Value(), 0, Rect{}, texels, 16).Ok());

    PipelineDesc desc = argbPipeline();
    desc.alphaTest = true;
    desc.alphaFunction = CompareFunction::kGreater;
    desc.cull = CullMode::kNone;
    desc.stages[0].colorOp = TextureOp::kSelectArg1;
    const std::vector<TestVertex> quad = {
        at(-0.5f, -0.5f, 0xFFFFFFFFu, 0.5f, 0, 0), at(3.5f, -0.5f, 0xFFFFFFFFu, 0.5f, 1, 0),
        at(-0.5f, 3.5f, 0xFFFFFFFFu, 0.5f, 0, 1),  at(3.5f, -0.5f, 0xFFFFFFFFu, 0.5f, 1, 0),
        at(3.5f, 3.5f, 0xFFFFFFFFu, 0.5f, 1, 1),   at(-0.5f, 3.5f, 0xFFFFFFFFu, 0.5f, 0, 1),
    };
    const TextureId target = makeTarget(gpu, 4, 4);
    DrawState state;
    state.textures[0] = texture.Value();
    state.alphaReference = 0x80;
    drawScene(gpu, target, makePipeline(gpu, desc), makeVertices(gpu, quad), 6, kNavy, state);
    const std::vector<std::uint32_t> pixels = readArgb(gpu, target, 4, 4);
    for (int y = 0; y < 4; ++y) {
        CHECK(pixels[y * 4 + 0] == kNavy);
        CHECK(pixels[y * 4 + 1] == kNavy);
        CHECK(pixels[y * 4 + 2] == 0xC000FF00u);
        CHECK(pixels[y * 4 + 3] == 0xC000FF00u);
    }
}

TEST_CASE("a blit between two indexed textures copies the indices, not the colours") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    const Palettised source = makePalettised(gpu);
    TextureDesc desc;
    desc.width = 4;
    desc.height = 4;
    desc.format = PixelFormat::kIndexed8;
    auto destination = gpu.CreateTexture(desc); // no palette of its own
    REQUIRE(destination.Ok());
    REQUIRE(gpu.Fill(destination.Value(), Rect{}, 7).Ok());
    BlitDesc blit;
    blit.source = source.texture;
    blit.destination = destination.Value();
    REQUIRE(gpu.Blit(blit).Ok());
    std::uint8_t indices[16] = {};
    REQUIRE(gpu.ReadTexture(destination.Value(), 0, Rect{}, indices, 4).Ok());
    const std::uint8_t expected[16] = {0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1, 0, 1, 1, 0};
    for (int i = 0; i < 16; ++i) {
        CHECK(indices[i] == expected[i]);
    }

    // With the source colour key, index 0 stays what the destination held.
    REQUIRE(gpu.Fill(destination.Value(), Rect{}, 7).Ok());
    blit.sourceColorKey = true;
    REQUIRE(gpu.Blit(blit).Ok());
    REQUIRE(gpu.ReadTexture(destination.Value(), 0, Rect{}, indices, 4).Ok());
    const std::uint8_t keyed[16] = {7, 1, 1, 7, 1, 2, 2, 1, 1, 2, 2, 1, 7, 1, 1, 7};
    for (int i = 0; i < 16; ++i) {
        CHECK(indices[i] == keyed[i]);
    }
}

// Render scales. A texture with scale k holds k*k physical pixels per logical pixel. A blit of a
// whole k-scaled texture into a k=1 texture k times its size samples one physical texel per
// destination pixel (point sampling, texel centres line up), so it shows the physical image.

namespace {

TextureId makeScaledTarget(Gpu &gpu, std::uint32_t width, std::uint32_t height,
                           std::uint8_t scale, PixelFormat format = PixelFormat::kArgb8888) {
    TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.renderTarget = true;
    desc.renderScale = scale;
    auto made = gpu.CreateTexture(desc);
    REQUIRE(made.Ok());
    return made.Value();
}

// The physical image of a whole k-scaled texture, read through a blit into a k=1 texture.
std::vector<std::uint32_t> readPhysical(Gpu &gpu, TextureId scaled, std::uint32_t width,
                                        std::uint32_t height, std::uint32_t scale) {
    const TextureId copy = makeTarget(gpu, width * scale, height * scale);
    BlitDesc blit;
    blit.source = scaled;
    blit.sourceRect = Rect{0, 0, width, height};
    blit.destination = copy;
    blit.destinationRect = Rect{0, 0, width * scale, height * scale};
    REQUIRE(gpu.Blit(blit).Ok());
    return readArgb(gpu, copy, width * scale, height * scale);
}

} // namespace

TEST_CASE("a triangle drawn at render scale 3 is the same triangle rasterised 3 times larger") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    const PipelineId pipeline = makePipeline(gpu, argbPipeline());

    // Logical vertex p lands on physical pixel position 3 * (p + 0.5), the same place as vertex
    // 3p + 1 on a k=1 target: (8, 8) is (25, 25), (56, 8) is (169, 25), (8, 56) is (25, 169).
    const TextureId scaled = makeScaledTarget(gpu, 64, 64, 3);
    drawScene(gpu, scaled, pipeline, makeVertices(gpu, greenTriangle()), 3, kNavy);
    const TextureId large = makeTarget(gpu, 192, 192);
    drawScene(gpu, large, pipeline,
              makeVertices(gpu, {at(25, 25, kGreen), at(169, 25, kGreen), at(25, 169, kGreen)}),
              3, kNavy);
    const std::vector<std::uint32_t> physical = readPhysical(gpu, scaled, 64, 64, 3);
    CHECK(physical == readArgb(gpu, large, 192, 192));

    // Read back, each 3x3 block is averaged: the k=1 image, except on the triangle's edges, where
    // a block partly inside is a mix of green and navy. Tolerance: a pixel may differ from the
    // k=1 image only where its 3x3 neighbourhood in the k=1 image holds both colours, and then
    // only as a mix (alpha 255, red 0, green + blue weights summing to one block).
    const TextureId plain = makeTarget(gpu, 64, 64);
    drawScene(gpu, plain, pipeline, makeVertices(gpu, greenTriangle()), 3, kNavy);
    const std::vector<std::uint32_t> reference = readArgb(gpu, plain, 64, 64);
    const std::vector<std::uint32_t> averaged = readArgb(gpu, scaled, 64, 64);
    const auto onEdge = [&reference](int x, int y) {
        bool green = false;
        bool navy = false;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const int nx = x + dx;
                const int ny = y + dy;
                if (nx < 0 || ny < 0 || nx >= 64 || ny >= 64) {
                    continue;
                }
                green |= reference[std::size_t(ny) * 64 + nx] == kGreen;
                navy |= reference[std::size_t(ny) * 64 + nx] == kNavy;
            }
        }
        return green && navy;
    };
    int differing = 0;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const std::uint32_t got = averaged[std::size_t(y) * 64 + x];
            if (got == reference[std::size_t(y) * 64 + x]) {
                continue;
            }
            ++differing;
            CAPTURE(x);
            CAPTURE(y);
            CHECK(onEdge(x, y));
            CHECK((got & 0xFFFF0000u) == 0xFF000000u);
            // n green physical pixels of 9: green = round(255n/9), blue = round(128(9-n)/9).
            const std::uint32_t green = (got >> 8) & 0xFF;
            const std::uint32_t blue = got & 0xFF;
            bool mix = false;
            for (std::uint32_t n = 0; n <= 9; ++n) {
                mix |= green == (255 * n + 4) / 9 && blue == (128 * (9 - n) + 4) / 9;
            }
            CHECK(mix);
        }
    }
    // The three edges are 48 + 48 + 68 pixels long; no more than that can be mixed.
    CHECK(differing <= 164);
    MESSAGE("edge pixels that differ at render scale 3: " << differing);
}

TEST_CASE("an upload and a read-back round-trip a pattern at render scale 2") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    const TextureId texture = makeScaledTarget(gpu, 4, 2, 2);
    const std::vector<std::uint32_t> pattern = {kRed,   kGreen, kBlue,  kNavy,
                                                kGreen, kNavy,  kRed,   0x80402010u};
    REQUIRE(gpu.WriteTexture(texture, 0, Rect{}, pattern.data(), 16).Ok());
    CHECK(readArgb(gpu, texture, 4, 2) == pattern);

    // Each logical pixel is a 2x2 block of physical pixels.
    const std::vector<std::uint32_t> expected = {
        kRed,   kRed,   kGreen, kGreen, kBlue, kBlue, kNavy,       kNavy,
        kRed,   kRed,   kGreen, kGreen, kBlue, kBlue, kNavy,       kNavy,
        kGreen, kGreen, kNavy,  kNavy,  kRed,  kRed,  0x80402010u, 0x80402010u,
        kGreen, kGreen, kNavy,  kNavy,  kRed,  kRed,  0x80402010u, 0x80402010u,
    };
    CHECK(readPhysical(gpu, texture, 4, 2, 2) == expected);

    // A region upload and a region read stay in logical pixels.
    const std::uint32_t one[1] = {kBlue};
    REQUIRE(gpu.WriteTexture(texture, 0, Rect{1, 1, 1, 1}, one, 4).Ok());
    std::uint32_t back[2] = {};
    REQUIRE(gpu.ReadTexture(texture, 0, Rect{0, 1, 2, 1}, back, 8).Ok());
    CHECK(back[0] == kGreen);
    CHECK(back[1] == kBlue);

    // 16-bit pixels too.
    const TextureId texture565 = makeScaledTarget(gpu, 2, 1, 2, PixelFormat::kRgb565);
    const std::uint16_t pixels565[2] = {0xF800, 0x07E0};
    REQUIRE(gpu.WriteTexture(texture565, 0, Rect{}, pixels565, 4).Ok());
    std::uint16_t back565[2] = {};
    REQUIRE(gpu.ReadTexture(texture565, 0, Rect{}, back565, 4).Ok());
    CHECK(back565[0] == 0xF800);
    CHECK(back565[1] == 0x07E0);
}

TEST_CASE("fills, clears, viewports, blits and depth stay logical at render scale 2") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    const TextureId target = makeScaledTarget(gpu, 8, 8, 2);
    REQUIRE(gpu.Fill(target, Rect{}, kNavy).Ok());
    REQUIRE(gpu.Fill(target, Rect{6, 0, 2, 1}, kRed).Ok());
    std::vector<std::uint32_t> pixels = readArgb(gpu, target, 8, 8);
    CHECK(pixels[0] == kNavy);
    CHECK(pixels[5] == kNavy);
    CHECK(pixels[6] == kRed);
    CHECK(pixels[7] == kRed);
    CHECK(pixels[8 + 6] == kNavy);

    // A clear rectangle and a viewport, both logical.
    const BufferId vertices = makeVertices(
        gpu, {at(-0.5f, -0.5f, kGreen), at(15.5f, -0.5f, kGreen), at(-0.5f, 15.5f, kGreen)});
    REQUIRE(gpu.BeginScene(RenderTargets{target, TextureId{}}).Ok());
    ClearDesc clear;
    clear.colorValue = kBlue;
    clear.area = Rect{0, 4, 8, 4};
    REQUIRE(gpu.Clear(clear).Ok());
    Viewport top;
    top.width = 4;
    top.height = 4;
    REQUIRE(gpu.SetViewport(top).Ok());
    DrawCall draw;
    draw.pipeline = makePipeline(gpu, argbPipeline());
    draw.vertices = vertices;
    draw.vertexCount = 3;
    REQUIRE(gpu.Draw(draw).Ok());
    REQUIRE(gpu.EndScene().Ok());
    pixels = readArgb(gpu, target, 8, 8);
    CHECK(pixels[0] == kGreen);
    CHECK(pixels[3 * 8 + 3] == kGreen);
    CHECK(pixels[4] == kNavy);       // right of the viewport
    CHECK(pixels[6] == kRed);        // the earlier fill
    CHECK(pixels[4 * 8 + 0] == kBlue); // the cleared lower half
    CHECK(pixels[7 * 8 + 7] == kBlue);

    // A k=1 sprite blitted in lands on logical pixels; a same-texture blit too.
    const TextureId sprite = makeTarget(gpu, 2, 2);
    REQUIRE(gpu.Fill(sprite, Rect{}, kRed).Ok());
    BlitDesc blit;
    blit.source = sprite;
    blit.sourceRect = Rect{0, 0, 2, 2};
    blit.destination = target;
    blit.destinationRect = Rect{5, 5, 2, 2};
    REQUIRE(gpu.Blit(blit).Ok());
    blit.source = target;
    blit.sourceRect = Rect{5, 5, 1, 1};
    blit.destinationRect = Rect{0, 7, 1, 1};
    REQUIRE(gpu.Blit(blit).Ok());
    pixels = readArgb(gpu, target, 8, 8);
    CHECK(pixels[5 * 8 + 5] == kRed);
    CHECK(pixels[6 * 8 + 6] == kRed);
    CHECK(pixels[4 * 8 + 4] == kBlue);
    CHECK(pixels[7 * 8 + 7] == kBlue);
    CHECK(pixels[7 * 8 + 0] == kRed);
    CHECK(pixels[7 * 8 + 1] == kBlue);

    // A depth target of the same scale works; one of another scale is refused.
    const TextureId depth = makeScaledTarget(gpu, 8, 8, 2, PixelFormat::kDepth16);
    REQUIRE(gpu.Fill(depth, Rect{}, 0).Ok());
    PipelineDesc depthDesc = argbPipeline();
    depthDesc.depthTarget = true;
    depthDesc.depthTest = true;
    depthDesc.depthFunction = CompareFunction::kLess;
    draw.pipeline = makePipeline(gpu, depthDesc);
    REQUIRE(gpu.BeginScene(RenderTargets{target, depth}).Ok());
    REQUIRE(gpu.Draw(draw).Ok()); // depth 0 everywhere: nothing passes "less"
    REQUIRE(gpu.EndScene().Ok());
    CHECK(readArgb(gpu, target, 8, 8)[7 * 8 + 7] == kBlue);
    REQUIRE(gpu.BeginScene(RenderTargets{target, depth}).Ok());
    ClearDesc depthClear;
    depthClear.depth = true;
    depthClear.colorValue = kNavy;
    REQUIRE(gpu.Clear(depthClear).Ok());
    REQUIRE(gpu.Draw(draw).Ok());
    REQUIRE(gpu.EndScene().Ok());
    CHECK(readArgb(gpu, target, 8, 8)[0] == kGreen);
    const TextureId flatDepth = makeTarget(gpu, 8, 8, PixelFormat::kDepth16);
    CHECK(gpu.BeginScene(RenderTargets{target, flatDepth}).Code() == Error::kInvalidArgument);
}

TEST_CASE("render scales are refused where they cannot apply") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    TextureDesc desc;
    desc.width = 64;
    desc.height = 64;
    desc.format = PixelFormat::kArgb8888;
    desc.renderTarget = true;
    desc.renderScale = 0;
    CHECK(gpu.CreateTexture(desc).Code() == Error::kInvalidArgument);
    desc.renderScale = 2;
    desc.mipLevels = 2;
    CHECK(gpu.CreateTexture(desc).Code() == Error::kInvalidArgument);
    desc.mipLevels = 1;
    desc.format = PixelFormat::kIndexed8;
    desc.renderTarget = false;
    CHECK(gpu.CreateTexture(desc).Code() == Error::kInvalidArgument);
    desc.format = PixelFormat::kArgb8888;
    desc.renderTarget = true;
    desc.width = 4097; // 8194 physical
    CHECK(gpu.CreateTexture(desc).Code() == Error::kInvalidArgument);
    desc.width = 4096;
    CHECK(gpu.CreateTexture(desc).Ok());

    CHECK(gpu.ConfigureScaledBackBuffer(640, 480, PixelFormat::kRgb565, 0).Code() ==
          Error::kInvalidArgument);
    CHECK(gpu.ConfigureScaledBackBuffer(4097, 480, PixelFormat::kRgb565, 2).Code() ==
          Error::kInvalidArgument);
    CHECK(gpu.ConfigureScaledBackBuffer(640, 480, PixelFormat::kIndexed8, 2).Code() ==
          Error::kInvalidArgument);
    CHECK(gpu.ConfigureScaledBackBuffer(640, 480, PixelFormat::kDepth16, 2).Code() ==
          Error::kInvalidArgument);
}

TEST_CASE("a back buffer at render scale 2 starts black and presents headless") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    auto back = gpu.ConfigureScaledBackBuffer(640, 480, PixelFormat::kRgb565, 2);
    REQUIRE(back.Ok());
    std::uint16_t corner[1] = {0xFFFF};
    REQUIRE(gpu.ReadTexture(back.Value(), 0, Rect{639, 479, 1, 1}, corner, 2).Ok());
    CHECK(corner[0] == 0x0000);
    REQUIRE(gpu.Fill(back.Value(), Rect{}, 0xF800).Ok());
    REQUIRE(gpu.ReadTexture(back.Value(), 0, Rect{639, 479, 1, 1}, corner, 2).Ok());
    CHECK(corner[0] == 0xF800);
    CHECK(gpu.Present().Ok());
}

TEST_CASE("sharp-bilinear enlarges by the whole part of the scale before the linear step") {
    using gpusdl::presentPrescale;
    CHECK(presentPrescale(TextureFilter::kLinear, 800, 600, 800, 600) == 1);   // 1x
    CHECK(presentPrescale(TextureFilter::kLinear, 800, 600, 1200, 900) == 1);  // 1.5x
    CHECK(presentPrescale(TextureFilter::kLinear, 800, 600, 1600, 1200) == 1); // 2x, nearest
    CHECK(presentPrescale(TextureFilter::kLinear, 800, 600, 2000, 1500) == 2); // 2.5x
    CHECK(presentPrescale(TextureFilter::kLinear, 800, 600, 1920, 1440) == 2); // 2.4x
    CHECK(presentPrescale(TextureFilter::kLinear, 640, 480, 2400, 1800) == 3); // 3.75x
    CHECK(presentPrescale(TextureFilter::kLinear, 800, 600, 1000, 750) == 1);  // 1.25x
    CHECK(presentPrescale(TextureFilter::kLinear, 800, 600, 400, 300) == 1);   // smaller
    CHECK(presentPrescale(TextureFilter::kPoint, 800, 600, 2000, 1500) == 1);
    CHECK(presentPrescale(TextureFilter::kLinear, 0, 600, 2000, 1500) == 1);
}

namespace {

// A 2x2 frame: a grey column (64) on the left, a white one (255) on the right.
constexpr std::uint32_t kGrey = 0xFF404040u;
constexpr std::uint32_t kWhite = 0xFFFFFFFFu;

TextureId makeColumns(Gpu &gpu) {
    const TextureId frame = makeTarget(gpu, 2, 2);
    const std::uint32_t pixels[4] = {kGrey, kWhite, kGrey, kWhite};
    REQUIRE(gpu.WriteTexture(frame, 0, Rect{}, pixels, 8).Ok());
    return frame;
}

// The red channel of row 0 after the frame is scaled into `place` of a target of that width
// plus a black bar column on each side, and `place.height` rows.
std::vector<int> scaledRow(Gpu &gpu, TextureId frame, std::uint32_t width, std::uint32_t height) {
    const TextureId target = makeTarget(gpu, width + 2, height);
    REQUIRE(gpusdl::scaleFrameInto(gpu, frame, target, Rect{1, 0, width, height}).Ok());
    const std::vector<std::uint32_t> pixels = readArgb(gpu, target, width + 2, height);
    std::vector<int> row;
    for (std::uint32_t x = 0; x < width + 2; ++x) {
        row.push_back(static_cast<int>((pixels[x] >> 16) & 0xFF));
    }
    return row;
}

// Each value within `tolerance` of the hand-worked one (the GPU's linear weights are not exact).
void checkRow(const std::vector<int> &got, const std::vector<int> &expected, int tolerance) {
    REQUIRE(got.size() == expected.size());
    for (std::size_t i = 0; i < got.size(); ++i) {
        CAPTURE(i);
        CHECK(got[i] >= expected[i] - tolerance);
        CHECK(got[i] <= expected[i] + tolerance);
    }
}

} // namespace

// Hand-worked values. A destination pixel x samples the frame at u = (x + 0.5) * frameW / placeW
// texels; linear weighs the two texel centres (at 0.5 and 1.5) around u, clamped at the edges.
// The first and last values of each row are the black bars beside the place.
TEST_CASE("the present filters sample a scaled frame as worked out by hand") {
    Rig rig;
    REQUIRE_GPU(rig);
    Gpu &gpu = *rig.gpu;
    const TextureId frame = makeColumns(gpu);

    SUBCASE("1x: the frame's own pixels with either filter") {
        REQUIRE(gpu.SetPresentFilter(TextureFilter::kPoint).Ok());
        checkRow(scaledRow(gpu, frame, 2, 2), {0, 64, 255, 0}, 0);
        REQUIRE(gpu.SetPresentFilter(TextureFilter::kLinear).Ok());
        checkRow(scaledRow(gpu, frame, 2, 2), {0, 64, 255, 0}, 0);
    }
    SUBCASE("2x: a whole-number scale is nearest with either filter") {
        REQUIRE(gpu.SetPresentFilter(TextureFilter::kPoint).Ok());
        checkRow(scaledRow(gpu, frame, 4, 4), {0, 64, 64, 255, 255, 0}, 0);
        REQUIRE(gpu.SetPresentFilter(TextureFilter::kLinear).Ok());
        checkRow(scaledRow(gpu, frame, 4, 4), {0, 64, 64, 255, 255, 0}, 0);
    }
    SUBCASE("1.5x: under 2x sharp-bilinear is plain linear") {
        // u = 0.33, 1.0, 1.67: the middle pixel is half of each column, (64 + 255) / 2.
        REQUIRE(gpu.SetPresentFilter(TextureFilter::kLinear).Ok());
        checkRow(scaledRow(gpu, frame, 3, 3), {0, 64, 160, 255, 0}, 2);
        // Nearest: the outer pixels are their columns; the middle one sits on the boundary.
        REQUIRE(gpu.SetPresentFilter(TextureFilter::kPoint).Ok());
        const std::vector<int> point = scaledRow(gpu, frame, 3, 3);
        CHECK(point[1] == 64);
        CHECK((point[2] == 64 || point[2] == 255));
        CHECK(point[3] == 255);
    }
    SUBCASE("2.5x: sharp-bilinear blends one pixel where linear blends three") {
        // Sharp-bilinear: nearest to 4 pixels (64 64 255 255), then linear 4 to 5:
        // u = 0.4, 1.2, 2.0, 2.8, 3.6 gives 64, 64, (64 + 255) / 2, 255, 255.
        REQUIRE(gpu.SetPresentFilter(TextureFilter::kLinear).Ok());
        checkRow(scaledRow(gpu, frame, 5, 5), {0, 64, 64, 160, 255, 255, 0}, 2);
        // Nearest: u = 0.2, 0.6, 1.0 (the boundary), 1.4, 1.8.
        REQUIRE(gpu.SetPresentFilter(TextureFilter::kPoint).Ok());
        const std::vector<int> point = scaledRow(gpu, frame, 5, 5);
        CHECK(point[1] == 64);
        CHECK(point[2] == 64);
        CHECK(point[4] == 255);
        CHECK(point[5] == 255);
    }
}

TEST_CASE("the present scaling into a texture refuses a Gpu that is not an SDL one") {
    auto stub = makeGpuStub();
    CHECK(gpusdl::scaleFrameInto(*stub, TextureId{1}, TextureId{2}, Rect{}).Code() ==
          Error::kInvalidArgument);
}

} // namespace sfcop::platform
