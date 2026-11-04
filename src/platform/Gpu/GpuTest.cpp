#include "platform/Gpu/Gpu.h"
#include "platform/Gpu/GpuMock.h"
#include "platform/Window/WindowMock.h"

#include <doctest/doctest.h>

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

} // namespace sfcop::platform
