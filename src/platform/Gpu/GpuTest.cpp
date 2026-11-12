#include "platform/Gpu/Gpu.h"
#include "platform/Gpu/GpuMock.h"
#include "platform/Window/WindowMock.h"

#include <doctest/doctest.h>

#include <map>
#include <string>
#include <vector>

namespace sfcop::platform {
namespace {

// The transformed, lit vertex the game draws (32 bytes).
struct TestVertex {
    float x, y, z, rhw;
    std::uint32_t diffuse;
    std::uint32_t specular;
    float u, v;
};

// A caller written against the interface only, as one frame of the game's 2D line drawing is:
// uploads two vertices, draws a line list into the back buffer, presents.
Status drawLine(Gpu &gpu, TextureId backBuffer, BufferId buffer, PipelineId pipeline) {
    const TestVertex line[2] = {
        {10.0f, 20.0f, 0.5f, 1.0f, 0xFFFF0000, 0xFF000000, 0.0f, 0.0f},
        {110.0f, 20.0f, 0.5f, 1.0f, 0xFF00FF00, 0xFF000000, 1.0f, 0.0f},
    };
    Status status = gpu.WriteBuffer(buffer, 0, line, sizeof line);
    if (status.Ok()) {
        status = gpu.BeginScene(RenderTargets{backBuffer, TextureId{}});
    }
    if (!status.Ok()) {
        return status;
    }
    ClearDesc clear;
    Status cleared = gpu.Clear(clear);
    DrawCall draw;
    draw.pipeline = pipeline;
    draw.vertices = buffer;
    draw.vertexCount = 2;
    Status drawn = cleared.Ok() ? gpu.Draw(draw) : cleared;
    Status ended = gpu.EndScene();
    if (!drawn.Ok()) {
        return drawn;
    }
    if (!ended.Ok()) {
        return ended;
    }
    return gpu.Present();
}

} // namespace

TEST_CASE("the game's transformed and lit vertex layout is 32 bytes") {
    VertexLayout layout;
    CHECK(vertexStride(layout) == 32);
    CHECK(sizeof(TestVertex) == 32);
    VertexLayout untransformed;
    untransformed.position = PositionKind::kXyz;
    untransformed.normal = true;
    untransformed.specular = false;
    untransformed.texCoordSets = 2;
    CHECK(vertexStride(untransformed) == 12 + 12 + 4 + 16);
}

TEST_CASE("pixel formats have the sizes DirectDraw surfaces use") {
    CHECK(pixelBytes(PixelFormat::kRgb565) == 2);
    CHECK(pixelBytes(PixelFormat::kArgb1555) == 2);
    CHECK(pixelBytes(PixelFormat::kXrgb8888) == 4);
    CHECK(pixelBytes(PixelFormat::kArgb8888) == 4);
    CHECK(pixelBytes(PixelFormat::kIndexed8) == 1);
    CHECK(pixelBytes(PixelFormat::kDepth16) == 2);
}

TEST_CASE("a default pipeline has stage 0 modulating and stage 1 disabled, as Direct3D starts") {
    PipelineDesc desc;
    CHECK(desc.stages[0].colorOp == TextureOp::kModulate);
    CHECK(desc.stages[0].alphaOp == TextureOp::kSelectArg1);
    CHECK(desc.stages[1].colorOp == TextureOp::kDisable);
    CHECK(desc.stages[1].alphaOp == TextureOp::kDisable);
    CHECK(desc.stages[1].texCoordIndex == 1);
    CHECK(desc.cull == CullMode::kCounterClockwise);
}

TEST_CASE("a caller draws and presents one frame through the gpu interface") {
    WindowMock window;
    GpuMock mock;
    Gpu &gpu = mock;
    REQUIRE(gpu.Attach(window).Ok());
    TextureId backBuffer = gpu.ConfigureBackBuffer(800, 600, PixelFormat::kRgb565).Value();
    BufferId buffer = gpu.CreateBuffer(BufferKind::kVertex, 64).Value();
    PipelineDesc desc;
    desc.primitive = PrimitiveType::kLineList;
    PipelineId pipeline = gpu.CreatePipeline(desc).Value();
    mock.log.Clear();

    CHECK(drawLine(gpu, backBuffer, buffer, pipeline).Ok());
    CHECK(mock.lastTargets.color == TextureId{1});
    CHECK(mock.lastClear.colorValue == 0xFF000000);
    CHECK(mock.lastDraw.state.textures[0] == TextureId{});
    CHECK(mock.log.Calls() == std::vector<std::string>{
                                  "WriteBuffer(1, 0, 64)",
                                  "BeginScene(1, 0)",
                                  "Clear(true, false)",
                                  "Draw(1, 1, 0, 2, 0, 0)",
                                  "EndScene()",
                                  "Present()",
                              });
}

TEST_CASE("a failed draw still ends the scene and reaches the caller") {
    GpuMock mock;
    mock.failWith = Error::kInvalidArgument;
    mock.failOnly = "Draw";
    CHECK(drawLine(mock, TextureId{1}, BufferId{1}, PipelineId{1}).Code() ==
          Error::kInvalidArgument);
    CHECK(mock.log.Calls() == std::vector<std::string>{
                                  "WriteBuffer(1, 0, 64)",
                                  "BeginScene(1, 0)",
                                  "Clear(true, false)",
                                  "Draw(1, 1, 0, 2, 0, 0)",
                                  "EndScene()",
                              });
}

TEST_CASE("a refused upload stops the frame before a scene begins") {
    GpuMock mock;
    mock.failWith = Error::kWrongState;
    CHECK(drawLine(mock, TextureId{1}, BufferId{1}, PipelineId{1}).Code() == Error::kWrongState);
    CHECK(mock.log.Calls() == std::vector<std::string>{"WriteBuffer(1, 0, 64)"});
}

TEST_CASE("readback returns the programmed pixels for the requested region") {
    GpuMock mock;
    Gpu &gpu = mock;
    mock.readValue = 0xAB;
    std::uint8_t pixels[4 * 2] = {};
    CHECK(gpu.ReadTexture(TextureId{1}, 0, Rect{0, 0, 2, 2}, pixels, 4).Ok());
    CHECK(pixels[0] == 0xAB);
    CHECK(pixels[7] == 0xAB);
    CHECK(gpu.SetColorKey(TextureId{3}, std::nullopt).Ok());
    CHECK(gpu.SetColorKey(TextureId{3}, 0xF81Fu).Ok());
    CHECK(mock.log.Calls() == std::vector<std::string>{
                                  "ReadTexture(1, 0, 0, 0, 2, 2, 4)",
                                  "SetColorKey(3, \"none\")",
                                  "SetColorKey(3, 63519)",
                              });
}

TEST_CASE("every method of the gpu stub reports unsupported") {
    std::unique_ptr<Gpu> gpu = makeGpuStub();
    WindowMock window;
    TextureId texture{1};
    BufferId buffer{1};
    PaletteId palette{1};
    std::uint8_t data[16] = {};
    PaletteEntry entries[kPaletteSize] = {};
    CHECK(gpu->Attach(window).Code() == Error::kUnsupported);
    CHECK(gpu->Detach().Code() == Error::kUnsupported);
    CHECK(gpu->GetCaps().Code() == Error::kUnsupported);
    CHECK(gpu->ConfigureBackBuffer(640, 480, PixelFormat::kRgb565).Code() ==
          Error::kUnsupported);
    CHECK(gpu->CreateBuffer(BufferKind::kIndex, 16).Code() == Error::kUnsupported);
    CHECK(gpu->WriteBuffer(buffer, 0, data, 16).Code() == Error::kUnsupported);
    CHECK(gpu->DestroyBuffer(buffer).Code() == Error::kUnsupported);
    CHECK(gpu->CreateTexture(TextureDesc{}).Code() == Error::kUnsupported);
    CHECK(gpu->WriteTexture(texture, 0, Rect{}, data, 4).Code() == Error::kUnsupported);
    CHECK(gpu->ReadTexture(texture, 0, Rect{}, data, 4).Code() == Error::kUnsupported);
    CHECK(gpu->DestroyTexture(texture).Code() == Error::kUnsupported);
    CHECK(gpu->SetColorKey(texture, 0u).Code() == Error::kUnsupported);
    CHECK(gpu->CreatePalette(entries).Code() == Error::kUnsupported);
    CHECK(gpu->WritePalette(palette, 0, 1, entries).Code() == Error::kUnsupported);
    CHECK(gpu->DestroyPalette(palette).Code() == Error::kUnsupported);
    CHECK(gpu->AttachPalette(texture, palette).Code() == Error::kUnsupported);
    CHECK(gpu->CreatePipeline(PipelineDesc{}).Code() == Error::kUnsupported);
    CHECK(gpu->DestroyPipeline(PipelineId{1}).Code() == Error::kUnsupported);
    CHECK(gpu->BeginScene(RenderTargets{}).Code() == Error::kUnsupported);
    CHECK(gpu->SetViewport(Viewport{}).Code() == Error::kUnsupported);
    CHECK(gpu->Clear(ClearDesc{}).Code() == Error::kUnsupported);
    CHECK(gpu->Draw(DrawCall{}).Code() == Error::kUnsupported);
    CHECK(gpu->EndScene().Code() == Error::kUnsupported);
    CHECK(gpu->Blit(BlitDesc{}).Code() == Error::kUnsupported);
    CHECK(gpu->Fill(texture, Rect{}, 0).Code() == Error::kUnsupported);
    CHECK(gpu->Present().Code() == Error::kUnsupported);
}

namespace {

void checkRect(const Rect &got, std::int32_t x, std::int32_t y, std::uint32_t width,
               std::uint32_t height) {
    CHECK(got.x == x);
    CHECK(got.y == y);
    CHECK(got.width == width);
    CHECK(got.height == height);
}

// A Gpu written before SetPresentFilter existed: it overrides every other method.
struct OlderGpu final : Gpu {
    Status Attach(Window &) override { return {}; }
    Status Detach() override { return {}; }
    Result<GpuCaps> GetCaps() override { return GpuCaps{}; }
    Result<TextureId> ConfigureBackBuffer(std::uint32_t, std::uint32_t, PixelFormat) override {
        return TextureId{1};
    }
    Result<BufferId> CreateBuffer(BufferKind, std::uint32_t) override { return BufferId{1}; }
    Status WriteBuffer(BufferId, std::uint32_t, const void *, std::uint32_t) override { return {}; }
    Status DestroyBuffer(BufferId) override { return {}; }
    Result<TextureId> CreateTexture(const TextureDesc &) override { return TextureId{2}; }
    Status WriteTexture(TextureId, std::uint8_t, Rect, const void *, std::uint32_t) override {
        return {};
    }
    Status ReadTexture(TextureId, std::uint8_t, Rect, void *, std::uint32_t) override { return {}; }
    Status DestroyTexture(TextureId) override { return {}; }
    Status SetColorKey(TextureId, std::optional<std::uint32_t>) override { return {}; }
    Result<PaletteId> CreatePalette(const PaletteEntry *) override { return PaletteId{1}; }
    Status WritePalette(PaletteId, std::uint32_t, std::uint32_t, const PaletteEntry *) override {
        return {};
    }
    Status DestroyPalette(PaletteId) override { return {}; }
    Status AttachPalette(TextureId, PaletteId) override { return {}; }
    Result<PipelineId> CreatePipeline(const PipelineDesc &) override { return PipelineId{1}; }
    Status DestroyPipeline(PipelineId) override { return {}; }
    Status BeginScene(const RenderTargets &) override { return {}; }
    Status SetViewport(const Viewport &) override { return {}; }
    Status Clear(const ClearDesc &) override { return {}; }
    Status Draw(const DrawCall &) override { return {}; }
    Status EndScene() override { return {}; }
    Status Blit(const BlitDesc &) override { return {}; }
    Status Fill(TextureId, Rect, std::uint32_t) override { return {}; }
    Status Present() override { return {}; }
};

} // namespace

TEST_CASE("the frame is placed centred with its aspect kept in a smaller, equal or wider target") {
    // 1024x768 into 800x500 (smaller, wider aspect): 500 high, 1024*500/768 = 666.7 -> 667 wide.
    checkRect(presentRect(1024, 768, 800, 500), 66, 0, 667, 500);
    // The frame's own size: the whole target, pixel for pixel.
    checkRect(presentRect(1024, 768, 1024, 768), 0, 0, 1024, 768);
    // 1920x1080 (wider): 1080 high, 1440 wide, bars of 240 at each side.
    checkRect(presentRect(1024, 768, 1920, 1080), 240, 0, 1440, 1080);
    // Taller than the frame: bars above and below.
    checkRect(presentRect(1024, 768, 1024, 1024), 0, 128, 1024, 768);
    // 640x480 into 1366x768: 768 high, 1024 wide, 342 left over, 171 at the left.
    checkRect(presentRect(640, 480, 1366, 768), 171, 0, 1024, 768);
    // Twice the frame: exact.
    checkRect(presentRect(640, 480, 1280, 960), 0, 0, 1280, 960);
}

TEST_CASE("the whole-number placement takes the largest whole scale that fits, centred") {
    // 640x480 into 1920x1080: 3 across, 2 down, so 2: 1280x960, bars of 320 and 60.
    checkRect(presentRectWhole(640, 480, 1920, 1080), 320, 60, 1280, 960);
    // 800x600 into 1920x1080: 2 across, 1 down, so 1: the frame's own size, centred.
    checkRect(presentRectWhole(800, 600, 1920, 1080), 560, 240, 800, 600);
    // 800x600 into 2560x1440: 3 across, 2 down, so 2: 1600x1200 at 480, 120.
    checkRect(presentRectWhole(800, 600, 2560, 1440), 480, 120, 1600, 1200);
    // Exactly twice: the whole target.
    checkRect(presentRectWhole(640, 480, 1280, 960), 0, 0, 1280, 960);
    // Larger than the target (scale 0): as presentRect, 800x600 into 640x480 at 0, 0.
    checkRect(presentRectWhole(800, 600, 640, 480), 0, 0, 640, 480);
    // 1024x768 into 800x500: 0 whole, so presentRect's 667x500 at 66.
    checkRect(presentRectWhole(1024, 768, 800, 500), 66, 0, 667, 500);
    checkRect(presentRectWhole(0, 480, 1920, 1080), 0, 0, 0, 0);
    checkRect(presentRectWhole(640, 480, 1920, 0), 0, 0, 0, 0);
}

TEST_CASE("one chooser places the frame for the Gpu and the mouse alike") {
    checkRect(framePlacement(640, 480, 1920, 1080, false), 240, 0, 1440, 1080);
    checkRect(framePlacement(640, 480, 1920, 1080, true), 320, 60, 1280, 960);
    checkRect(framePlacement(1024, 768, 1024, 768, true), 0, 0, 1024, 768);
    checkRect(framePlacement(1024, 768, 1024, 768, false), 0, 0, 1024, 768);
}

TEST_CASE("a zero size anywhere places nothing") {
    checkRect(presentRect(0, 768, 800, 600), 0, 0, 0, 0);
    checkRect(presentRect(1024, 0, 800, 600), 0, 0, 0, 0);
    checkRect(presentRect(1024, 768, 0, 600), 0, 0, 0, 0);
    checkRect(presentRect(1024, 768, 800, 0), 0, 0, 0, 0);
}

TEST_CASE("presentRect is usable at compile time") {
    constexpr Rect place = presentRect(640, 480, 1280, 1200);
    static_assert(place.x == 0 && place.y == 120 && place.width == 1280 && place.height == 960,
                  "640x480 into 1280x1200 is 1280x960 with 120-point bars");
    CHECK(place.height == 960);
}

TEST_CASE("the mock records the present filter, the stub and an older Gpu refuse it") {
    GpuMock mock;
    CHECK(mock.SetPresentFilter(TextureFilter::kPoint).Ok());
    CHECK(mock.SetPresentFilter(TextureFilter::kLinear).Ok());
    CHECK(mock.log.Calls() ==
          std::vector<std::string>{"SetPresentFilter(0)", "SetPresentFilter(1)"});
    auto stub = makeGpuStub();
    CHECK(stub->SetPresentFilter(TextureFilter::kLinear).Code() == Error::kUnsupported);
    OlderGpu older;
    CHECK(older.SetPresentFilter(TextureFilter::kLinear).Code() == Error::kUnsupported);
}

TEST_CASE("the mock keeps each texture's render scale and logs scale 1 as before") {
    GpuMock mock;
    TextureDesc desc;
    desc.width = 800;
    desc.height = 600;
    desc.format = PixelFormat::kRgb565;
    desc.renderTarget = true;
    mock.createTextureResult = TextureId{5};
    REQUIRE(mock.CreateTexture(desc).Ok());
    desc.renderScale = 3;
    mock.createTextureResult = TextureId{6};
    REQUIRE(mock.CreateTexture(desc).Ok());
    mock.backBufferResult = TextureId{7};
    REQUIRE(mock.ConfigureScaledBackBuffer(800, 600, PixelFormat::kRgb565, 1).Ok());
    mock.backBufferResult = TextureId{8};
    REQUIRE(mock.ConfigureScaledBackBuffer(800, 600, PixelFormat::kRgb565, 2).Ok());
    CHECK(mock.log.Calls() == std::vector<std::string>{
                                  "CreateTexture(800, 600, 0)",
                                  "CreateTexture(800, 600, 0, \"scale\", 3)",
                                  "ConfigureBackBuffer(800, 600, 0)",
                                  "ConfigureScaledBackBuffer(800, 600, 0, 2)",
                              });
    CHECK(mock.textureScales == std::map<std::uint32_t, std::uint8_t>{{5, 1}, {6, 3}, {7, 1}, {8, 2}});
    CHECK(mock.lastTextureDesc.renderScale == 3);
}

TEST_CASE("a Gpu without render scales configures scale 1 and refuses others") {
    OlderGpu older;
    auto plain = older.ConfigureScaledBackBuffer(640, 480, PixelFormat::kRgb565, 1);
    REQUIRE(plain.Ok());
    CHECK(plain.Value() == TextureId{1});
    CHECK(older.ConfigureScaledBackBuffer(640, 480, PixelFormat::kRgb565, 2).Code() ==
          Error::kUnsupported);
    auto stub = makeGpuStub();
    CHECK(stub->ConfigureScaledBackBuffer(640, 480, PixelFormat::kRgb565, 1).Code() ==
          Error::kUnsupported);
    CHECK(stub->ConfigureScaledBackBuffer(640, 480, PixelFormat::kRgb565, 2).Code() ==
          Error::kUnsupported);
    CHECK(TextureDesc{}.renderScale == 1);
}

} // namespace sfcop::platform
