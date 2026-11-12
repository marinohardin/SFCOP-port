#include "hle/d3d3/D3d3TestRig.h"

namespace sfcop::hle::d3d3 {

using platform::PixelFormat;
using platform::TextureId;
using test::Calls;
using test::D3dRig;
using test::TlVertex;

namespace {

constexpr std::uint32_t kTriangleList = 4;
constexpr std::uint32_t kDdflipWait = 0x1; // DDFLIP_WAIT

const TlVertex kTriangle[4] = {
    {10.0f, 10.0f, 0.5f, 1.0f, 0xFFFF0000, 0xFF000000, 0.0f, 0.0f},
    {100.0f, 10.0f, 0.5f, 1.0f, 0xFF00FF00, 0xFF000000, 1.0f, 0.0f},
    {10.0f, 100.0f, 0.5f, 1.0f, 0xFF0000FF, 0xFF000000, 0.0f, 1.0f},
    {100.0f, 100.0f, 0.5f, 1.0f, 0xFFFFFFFF, 0xFF000000, 1.0f, 1.0f},
};

// Gives the back buffer texture 5 and the z-buffer texture 6.
void giveTextures(D3dRig &rig) {
    rig.gpu.createTextureResult = TextureId{5};
    REQUIRE(rig.back->GpuTexture().Ok()); // a surface's texture is made at its first Gpu use
    rig.gpu.createTextureResult = TextureId{6};
    ddraw4::BltFx fx;
    fx.fillColor = 0xFFFF;
    REQUIRE(rig.z->Blt(nullptr, nullptr, nullptr, ddraw4::kDdbltColorFill, &fx) == kDdOk);
    rig.gpu.createTextureResult = TextureId{7};
    rig.gpu.log.Clear();
}

void checkStage(const platform::TextureStage &actual, const platform::TextureStage &expected) {
    CHECK(actual.colorOp == expected.colorOp);
    CHECK(actual.colorArg1 == expected.colorArg1);
    CHECK(actual.colorArg2 == expected.colorArg2);
    CHECK(actual.alphaOp == expected.alphaOp);
    CHECK(actual.alphaArg1 == expected.alphaArg1);
    CHECK(actual.alphaArg2 == expected.alphaArg2);
    CHECK(actual.addressU == expected.addressU);
    CHECK(actual.addressV == expected.addressV);
    CHECK(actual.magFilter == expected.magFilter);
    CHECK(actual.minFilter == expected.minFilter);
    CHECK(actual.mipFilter == expected.mipFilter);
    CHECK(actual.texCoordIndex == expected.texCoordIndex);
}

void checkPipeline(const platform::PipelineDesc &actual, const platform::PipelineDesc &expected) {
    CHECK(actual.layout.position == expected.layout.position);
    CHECK(actual.layout.normal == expected.layout.normal);
    CHECK(actual.layout.diffuse == expected.layout.diffuse);
    CHECK(actual.layout.specular == expected.layout.specular);
    CHECK(actual.layout.texCoordSets == expected.layout.texCoordSets);
    CHECK(actual.primitive == expected.primitive);
    CHECK(actual.colorFormat == expected.colorFormat);
    CHECK(actual.depthTarget == expected.depthTarget);
    CHECK(actual.blend == expected.blend);
    CHECK(actual.sourceBlend == expected.sourceBlend);
    CHECK(actual.destinationBlend == expected.destinationBlend);
    CHECK(actual.depthTest == expected.depthTest);
    CHECK(actual.depthWrite == expected.depthWrite);
    CHECK(actual.depthFunction == expected.depthFunction);
    CHECK(actual.alphaTest == expected.alphaTest);
    CHECK(actual.alphaFunction == expected.alphaFunction);
    CHECK(actual.cull == expected.cull);
    CHECK(actual.fill == expected.fill);
    CHECK(actual.shade == expected.shade);
    CHECK(actual.specular == expected.specular);
    CHECK(actual.colorKey == expected.colorKey);
    CHECK(actual.fog == expected.fog);
    checkStage(actual.stages[0], expected.stages[0]);
    checkStage(actual.stages[1], expected.stages[1]);
}

Texture *makeTexture(D3dRig &rig) {
    ddraw4::SurfaceDesc desc = ddraw4::test::plainDesc(64, 64, PixelFormat::kArgb1555);
    desc.caps.caps = ddraw4::kDdscapsTexture;
    ddraw4::Surface *surface = rig.Create(desc);
    Texture *texture = new Texture(*surface); // as QueryInterface for IDirect3DTexture2 makes it
    surface->Release();                       // the texture's reference keeps it
    return texture;
}

} // namespace

TEST_CASE("a render state sequence gives one pipeline description") {
    D3dRig rig;
    Device *device = rig.MakeDevice();
    Texture *texture = makeTexture(rig);
    REQUIRE(device->SetTexture(0, texture) == kDdOk);
    const std::pair<std::uint32_t, std::uint32_t> states[] = {
        {kRsZEnable, 1},
        {kRsZWriteEnable, 1},
        {kRsZFunc, 4},
        {kRsAlphaBlendEnable, 1},
        {kRsSrcBlend, 5},
        {kRsDestBlend, 6},
        {kRsCullMode, 1},
        {kRsAlphaTestEnable, 1},
        {kRsAlphaFunc, 5},
        {kRsAlphaRef, 0x10},
        {kRsShadeMode, 2},
        {kRsSpecularEnable, 1},
        {kRsColorKeyEnable, 1},
        {kRsFogEnable, 1},
        {kRsFogTableMode, 0},
        {kRsFogColor, 0x00203040},
        {26, 1}, // DITHERENABLE: kept, not drawn
    };
    for (const auto &[state, value] : states) {
        REQUIRE(device->SetRenderState(state, value) == kDdOk);
    }
    const std::uint32_t stages[][3] = {
        {0, kTssColorOp, kTopModulate},
        {0, kTssColorArg1, kTaTexture},
        {0, kTssColorArg2, kTaDiffuse},
        {0, kTssAlphaOp, kTopModulate},
        {0, kTssAlphaArg1, kTaTexture},
        {0, kTssAlphaArg2, kTaDiffuse},
        {0, kTssMagFilter, 2},
        {0, kTssMinFilter, 2},
        {0, kTssMipFilter, 1},
        {0, kTssAddressU, 3},
        {0, kTssAddressV, 3},
        {1, kTssColorOp, kTopAdd},
        {1, kTssColorArg1, kTaTexture},
        {1, kTssColorArg2, kTaCurrent},
        {1, kTssAlphaOp, kTopSelectArg2},
        {1, kTssAlphaArg2, kTaCurrent},
        {1, kTssTexCoordIndex, 1},
    };
    for (const auto &stage : stages) {
        REQUIRE(device->SetTextureStageState(stage[0], stage[1], stage[2]) == kDdOk);
    }

    platform::PipelineDesc expected;
    expected.layout = platform::VertexLayout{platform::PositionKind::kXyzRhw, false, true, true, 1};
    expected.primitive = platform::PrimitiveType::kTriangleList;
    expected.colorFormat = PixelFormat::kRgb565;
    expected.depthTarget = true;
    expected.blend = true;
    expected.sourceBlend = platform::BlendFactor::kSourceAlpha;
    expected.destinationBlend = platform::BlendFactor::kInverseSourceAlpha;
    expected.depthTest = true;
    expected.depthWrite = true;
    expected.depthFunction = platform::CompareFunction::kLessEqual;
    expected.alphaTest = true;
    expected.alphaFunction = platform::CompareFunction::kGreater;
    expected.cull = platform::CullMode::kNone;
    expected.fill = platform::FillMode::kSolid;
    expected.shade = platform::ShadeMode::kGouraud;
    expected.specular = true;
    expected.colorKey = true;
    expected.fog = platform::FogMode::kVertex;
    expected.stages[0] =
        platform::TextureStage{platform::TextureOp::kModulate,   platform::TextureArg::kTexture,
                               platform::TextureArg::kDiffuse,   platform::TextureOp::kModulate,
                               platform::TextureArg::kTexture,   platform::TextureArg::kDiffuse,
                               platform::TextureAddress::kClamp, platform::TextureAddress::kClamp,
                               platform::TextureFilter::kLinear, platform::TextureFilter::kLinear,
                               platform::MipFilter::kNone,       0};
    // Stage 1 has no texture: where it would sample, it takes the previous stage's colour; its
    // coordinate set 1 is past the layout's one set.
    expected.stages[1] =
        platform::TextureStage{platform::TextureOp::kAdd,       platform::TextureArg::kCurrent,
                               platform::TextureArg::kCurrent,  platform::TextureOp::kSelectArg2,
                               platform::TextureArg::kCurrent,  platform::TextureArg::kCurrent,
                               platform::TextureAddress::kWrap, platform::TextureAddress::kWrap,
                               platform::TextureFilter::kPoint, platform::TextureFilter::kPoint,
                               platform::MipFilter::kNone,      0};

    checkPipeline(device->CurrentPipeline(expected.layout, expected.primitive), expected);
    const platform::DrawState state = device->CurrentDrawState();
    CHECK(state.alphaReference == 0x10);
    CHECK(state.fogColor == 0x203040u);
    CHECK(state.textureFactor == 0xFFFFFFFFu);

    giveTextures(rig);
    REQUIRE(device->BeginScene() == kDdOk);
    CHECK(device->DrawPrimitive(kTriangleList, kFvfTlVertex, kTriangle, 3, 0) == kDdOk);
    checkPipeline(rig.gpu.lastPipelineDesc, expected);
    CHECK(rig.gpu.lastDraw.state.textures[0].value == 7);
    CHECK(rig.gpu.lastDraw.state.textures[1].value == 0);
    REQUIRE(device->EndScene() == kDdOk);
    texture->Release();
    device->Release();
}

TEST_CASE("BeginScene draws into the back buffer and its z-buffer in the current viewport") {
    D3dRig rig;
    Device *device = rig.MakeDevice();
    Viewport *viewport = rig.MakeViewport(*device);
    giveTextures(rig);
    CHECK(device->BeginScene() == kDdOk);
    CHECK(rig.gpu.log.Calls() == Calls{"BeginScene(5, 6)", "SetViewport(0, 0, 800, 600)"});
    CHECK(device->BeginScene() == kD3dErrSceneInScene);
    CHECK(device->EndScene() == kDdOk);
    CHECK(device->EndScene() == kD3dErrSceneNotInScene);
    CHECK(rig.gpu.log.Calls().back() == "EndScene()");

    rig.gpu.log.Clear();
    ddraw4::SurfaceDesc locked;
    REQUIRE(rig.back->Lock(nullptr, locked, 0) == kDdOk); // nothing drawn: no read back
    REQUIRE(rig.back->Unlock() == kDdOk);
    CHECK(rig.gpu.log.Count("ReadTexture") == 0);
    viewport->Release();
    device->Release();
}

TEST_CASE("DrawPrimitive streams the game's vertices unchanged and keeps pipelines") {
    D3dRig rig;
    Device *device = rig.MakeDevice();
    giveTextures(rig);
    REQUIRE(device->BeginScene() == kDdOk);
    rig.gpu.log.Clear();
    CHECK(device->DrawPrimitive(kTriangleList, kFvfTlVertex, kTriangle, 3, 0) == kDdOk);
    CHECK(device->DrawPrimitive(kTriangleList, kFvfTlVertex, kTriangle, 3, 0) == kDdOk);
    CHECK(rig.gpu.log.Calls() == Calls{"CreatePipeline(3, 32)", "CreateBuffer(0, 1048576)",
                                       "WriteBuffer(1, 0, 96)", "Draw(1, 1, 0, 3, 0, 0)",
                                       "WriteBuffer(1, 96, 96)", "Draw(1, 1, 3, 3, 0, 0)"});
    REQUIRE(device->EndScene() == kDdOk);

    rig.gpu.log.Clear();
    ddraw4::SurfaceDesc locked;
    REQUIRE(rig.back->Lock(nullptr, locked, ddraw4::kDdlockReadOnly) == kDdOk);
    CHECK(rig.gpu.log.Calls() == Calls{"ReadTexture(5, 0, 0, 0, 800, 600, 1600)"});
    REQUIRE(rig.back->Unlock() == kDdOk);
    device->Release();
}

TEST_CASE("DrawPrimitive draws only whole primitives") {
    D3dRig rig;
    Device *device = rig.MakeDevice();
    giveTextures(rig);
    REQUIRE(device->BeginScene() == kDdOk);
    rig.gpu.log.Clear();
    CHECK(device->DrawPrimitive(kTriangleList, kFvfTlVertex, kTriangle, 4, 0) == kDdOk);
    CHECK(rig.gpu.log.Calls().back() == "Draw(1, 1, 0, 3, 0, 0)");
    rig.gpu.log.Clear();
    CHECK(device->DrawPrimitive(kTriangleList, kFvfTlVertex, kTriangle, 2, 0) == kDdOk);
    CHECK(rig.gpu.log.Calls().empty());
    REQUIRE(device->EndScene() == kDdOk);
    device->Release();
}

TEST_CASE("DrawIndexedPrimitive streams the indices beside the vertices") {
    D3dRig rig;
    Device *device = rig.MakeDevice();
    giveTextures(rig);
    REQUIRE(device->BeginScene() == kDdOk);
    rig.gpu.log.Clear();
    const std::uint16_t indices[6] = {0, 1, 2, 2, 1, 3};
    CHECK(device->DrawIndexedPrimitive(kTriangleList, kFvfTlVertex, kTriangle, 4, indices, 6, 0) ==
          kDdOk);
    CHECK(rig.gpu.log.Calls() == Calls{"CreatePipeline(3, 32)", "CreateBuffer(0, 1048576)",
                                       "WriteBuffer(1, 0, 128)", "CreateBuffer(1, 262144)",
                                       "WriteBuffer(1, 0, 12)", "Draw(1, 1, 0, 4, 1, 6)"});
    CHECK(device->DrawIndexedPrimitive(kTriangleList, kFvfTlVertex, kTriangle, 4, nullptr, 6, 0) ==
          ddraw4::kDdErrInvalidParams);
    REQUIRE(device->EndScene() == kDdOk);
    device->Release();
}

TEST_CASE("DrawPrimitive refuses draws outside a scene and formats it cannot draw") {
    D3dRig rig;
    Device *device = rig.MakeDevice();
    CHECK(device->DrawPrimitive(kTriangleList, kFvfTlVertex, kTriangle, 3, 0) ==
          kD3dErrSceneNotInScene);
    giveTextures(rig);
    REQUIRE(device->BeginScene() == kDdOk);
    CHECK(device->DrawPrimitive(7, kFvfTlVertex, kTriangle, 3, 0) == kD3dErrInvalidPrimitiveType);
    CHECK(device->DrawPrimitive(kTriangleList, 0x006 | kFvfDiffuse, kTriangle, 3, 0) ==
          kD3dErrInvalidVertexType); // D3DFVF_XYZB1
    CHECK(device->DrawPrimitive(kTriangleList, kFvfXyz | kFvfDiffuse, kTriangle, 3, 0) ==
          kD3dErrNoCurrentViewport);
    REQUIRE(device->EndScene() == kDdOk);
    CHECK(rig.gpu.log.Count("Draw") == 0);
    device->Release();
}

TEST_CASE("DrawPrimitive repacks untransformed vertices through the viewport") {
    D3dRig rig;
    Device *device = rig.MakeDevice();
    Viewport *viewport = rig.MakeViewport(*device);
    giveTextures(rig);
    REQUIRE(device->BeginScene() == kDdOk);
    rig.gpu.log.Clear();
    const float positions[9] = {0, 0, 0.5f, 1, 0, 0.5f, 0, 1, 0.5f};
    CHECK(device->DrawPrimitive(kTriangleList, kFvfXyz, positions, 3, 0) == kDdOk);
    CHECK(rig.gpu.log.Calls() == Calls{"CreatePipeline(3, 24)", "CreateBuffer(0, 1048576)",
                                       "WriteBuffer(1, 0, 72)", "Draw(1, 1, 0, 3, 0, 0)"});
    REQUIRE(device->EndScene() == kDdOk);
    viewport->Release();
    device->Release();
}

TEST_CASE("Clear2 outside a scene fills the back buffer and the z-buffer") {
    D3dRig rig;
    Device *device = rig.MakeDevice();
    Viewport *viewport = rig.MakeViewport(*device);
    giveTextures(rig);
    const std::vector<D3dRect> all{{0, 0, 800, 600}};
    CHECK(viewport->Clear2(all, kClearTarget | kClearZBuffer, 0xFF0000FF, 1.0f, 0) == kDdOk);
    CHECK(rig.gpu.log.Calls() ==
          Calls{"Fill(5, 0, 0, 800, 600, 31)", "Fill(6, 0, 0, 800, 600, 65535)"});
    rig.gpu.log.Clear();
    CHECK(ddraw4::test::readPixels16(*rig.back)[0] == 31); // from the host copy
    CHECK(rig.gpu.log.Count("ReadTexture") == 0);
    viewport->Release();
    device->Release();
}

TEST_CASE("Clear2 inside a scene clears the Gpu target within the viewport") {
    D3dRig rig;
    Device *device = rig.MakeDevice();
    Viewport *viewport = rig.MakeViewport(*device);
    ViewportDesc small;
    small.width = 200;
    small.height = 150;
    REQUIRE(viewport->SetViewport2(small) == kDdOk);
    giveTextures(rig);
    REQUIRE(device->BeginScene() == kDdOk);
    rig.gpu.log.Clear();
    const std::vector<D3dRect> rects{{100, 100, 300, 200}, {500, 500, 600, 600}};
    CHECK(viewport->Clear2(rects, kClearTarget, 0xFF102030, 1.0f, 0) == kDdOk);
    CHECK(rig.gpu.log.Calls() == Calls{"SetViewport(0, 0, 200, 150)", "Clear(true, false)"});
    CHECK(rig.gpu.lastClear.colorValue == 0xFF102030u);
    CHECK(rig.gpu.lastClear.area.x == 100);
    CHECK(rig.gpu.lastClear.area.y == 100);
    CHECK(rig.gpu.lastClear.area.width == 100);
    CHECK(rig.gpu.lastClear.area.height == 50);
    REQUIRE(device->EndScene() == kDdOk);
    viewport->Release();
    device->Release();
}

TEST_CASE("Clear2 inside a scene from a viewport at a non-zero origin") {
    D3dRig rig;
    Device *device = rig.MakeDevice(); // the current viewport is 800x600 at the origin
    Viewport *current = rig.MakeViewport(*device);
    Viewport *other = nullptr;
    REQUIRE(rig.d3d->CreateViewport(&other) == kDdOk);
    ViewportDesc desc;
    desc.x = 100;
    desc.y = 50;
    desc.width = 200;
    desc.height = 100;
    REQUIRE(other->SetViewport2(desc) == kDdOk);
    REQUIRE(device->AddViewport(other) == kDdOk);
    giveTextures(rig);
    REQUIRE(device->BeginScene() == kDdOk);
    rig.gpu.log.Clear();
    CHECK(other->Clear2({{120, 60, 400, 400}}, kClearTarget, 0xFF102030, 1.0f, 0) == kDdOk);
    CHECK(rig.gpu.log.Calls() == Calls{"SetViewport(100, 50, 200, 100)", "Clear(true, false)",
                                       "SetViewport(0, 0, 800, 600)"});
    // The area is in target pixels, clipped to the clearing viewport.
    CHECK(rig.gpu.lastClear.area.x == 120);
    CHECK(rig.gpu.lastClear.area.y == 60);
    CHECK(rig.gpu.lastClear.area.width == 180);
    CHECK(rig.gpu.lastClear.area.height == 90);
    REQUIRE(device->EndScene() == kDdOk);
    other->Release();
    current->Release();
    device->Release();
}

TEST_CASE("clearing depth needs a z-buffer") {
    D3dRig rig;
    REQUIRE(rig.back->DeleteAttachedSurface(0, rig.z) == kDdOk);
    Device *device = rig.MakeDevice();
    Viewport *viewport = rig.MakeViewport(*device);
    CHECK(viewport->Clear2({{0, 0, 10, 10}}, kClearZBuffer, 0, 1.0f, 0) ==
          kD3dErrZBufferNotPresent);
    std::uint32_t zEnable = 1;
    CHECK(device->GetRenderState(kRsZEnable, zEnable) == kDdOk);
    CHECK(zEnable == 0); // no z-buffer: off by default
    viewport->Release();
    device->Release();
}

TEST_CASE("ValidateDevice passes what the Gpu draws and names what it cannot") {
    D3dRig rig;
    Device *device = rig.MakeDevice();
    std::uint32_t passes = 0;
    CHECK(device->ValidateDevice(passes) == kDdOk);
    CHECK(passes == 1);
    REQUIRE(device->SetTextureStageState(0, kTssColorOp, 9) == kDdOk); // ADDSIGNED2X
    CHECK(device->ValidateDevice(passes) == kD3dErrUnsupportedColorOperation);
    CHECK(passes == 0);
    REQUIRE(device->SetTextureStageState(0, kTssColorOp, kTopModulate) == kDdOk);
    REQUIRE(device->SetTextureStageState(0, kTssColorArg2, kTaDiffuse | 0x10) == kDdOk);
    CHECK(device->ValidateDevice(passes) == kD3dErrUnsupportedColorArg);
    REQUIRE(device->SetTextureStageState(0, kTssColorArg2, kTaDiffuse) == kDdOk);
    REQUIRE(device->SetTextureStageState(0, kTssAlphaOp, 17) == kDdOk); // PREMODULATE
    CHECK(device->ValidateDevice(passes) == kD3dErrUnsupportedAlphaOperation);
    REQUIRE(device->SetTextureStageState(0, kTssAlphaOp, kTopSelectArg1) == kDdOk);
    REQUIRE(device->SetTextureStageState(1, kTssColorOp, kTopModulate) == kDdOk);
    REQUIRE(device->SetTextureStageState(1, kTssAlphaOp, kTopSelectArg2) == kDdOk);
    CHECK(device->ValidateDevice(passes) == kDdOk);
    REQUIRE(device->SetTextureStageState(2, kTssColorOp, kTopModulate) == kDdOk);
    CHECK(device->ValidateDevice(passes) == kD3dErrTooManyOperations);
    device->Release();
}

TEST_CASE("render states are kept, and the Direct3D 5 states set stage 0") {
    D3dRig rig;
    Device *device = rig.MakeDevice();
    std::uint32_t value = 0;
    CHECK(device->GetRenderState(kRsCullMode, value) == kDdOk);
    CHECK(value == 3);
    CHECK(device->SetRenderState(47, 2) == kDdOk); // ZBIAS: kept
    CHECK(device->GetRenderState(47, value) == kDdOk);
    CHECK(value == 2);
    CHECK(device->SetRenderState(0, 1) == ddraw4::kDdErrInvalidParams);
    CHECK(device->SetRenderState(kRenderStates, 1) == ddraw4::kDdErrInvalidParams);

    CHECK(device->SetRenderState(kRsTextureMapBlend, kTblendModulateAlpha) == kDdOk);
    CHECK(device->GetTextureStageState(0, kTssColorOp, value) == kDdOk);
    CHECK(value == kTopModulate);
    CHECK(device->GetTextureStageState(0, kTssAlphaOp, value) == kDdOk);
    CHECK(value == kTopModulate);
    CHECK(device->SetRenderState(kRsTextureMin, 6) == kDdOk); // LINEARMIPLINEAR
    CHECK(device->GetTextureStageState(0, kTssMinFilter, value) == kDdOk);
    CHECK(value == 2);
    CHECK(device->GetTextureStageState(0, kTssMipFilter, value) == kDdOk);
    CHECK(value == 3);
    CHECK(device->SetRenderState(kRsTextureAddress, 3) == kDdOk);
    CHECK(device->GetTextureStageState(0, kTssAddressV, value) == kDdOk);
    CHECK(value == 3);
    CHECK(device->SetTextureStageState(8, kTssColorOp, 1) == ddraw4::kDdErrInvalidParams);
    device->Release();
}

TEST_CASE("SetTexture holds the texture, and a texture handle sets stage 0") {
    D3dRig rig;
    Device *device = rig.MakeDevice();
    Texture *texture = makeTexture(rig);
    CHECK(device->SetTexture(0, texture) == kDdOk);
    CHECK(texture->RefCount() == 2);
    Texture *got = nullptr;
    CHECK(device->GetTexture(0, &got) == kDdOk);
    CHECK(got == texture);
    got->Release();
    CHECK(device->SetTexture(0, nullptr) == kDdOk);
    CHECK(texture->RefCount() == 1);

    std::uint32_t handle = 0;
    REQUIRE(texture->GetHandle(device, handle) == kDdOk);
    CHECK(device->SetRenderState(kRsTextureHandle, handle) == kDdOk);
    CHECK(device->GetTexture(0, &got) == kDdOk);
    REQUIRE(got != nullptr);
    CHECK(&got->Surface() == &texture->Surface()); // a view of the surface the handle names
    got->Release();
    CHECK(device->SetRenderState(kRsTextureHandle, 0) == kDdOk);
    CHECK(device->GetTexture(0, &got) == kDdOk);
    CHECK(got == nullptr);
    CHECK(device->SetTexture(8, texture) == ddraw4::kDdErrInvalidParams);
    texture->Release();
    device->Release();
}

TEST_CASE("transforms are kept and MultiplyTransform puts the given matrix first") {
    D3dRig rig;
    Device *device = rig.MakeDevice();
    Matrix out{};
    CHECK(device->GetTransform(kTransformWorld, out) == kDdOk);
    CHECK(out == kIdentity);
    const Matrix scale{2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1};
    const Matrix move{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 6, 7, 1};
    CHECK(device->SetTransform(kTransformView, move) == kDdOk);
    CHECK(device->MultiplyTransform(kTransformView, scale) == kDdOk);
    CHECK(device->GetTransform(kTransformView, out) == kDdOk);
    CHECK(out == Matrix{2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 5, 6, 7, 1});
    CHECK(device->SetTransform(4, move) == ddraw4::kDdErrInvalidParams);

    std::uint32_t light = 7;
    CHECK(device->SetLightState(2, 0x00404040) == kDdOk); // AMBIENT: kept
    CHECK(device->GetLightState(2, light) == kDdOk);
    CHECK(light == 0x00404040u);
    CHECK(device->SetLightState(9, 0) == ddraw4::kDdErrInvalidParams);
    device->Release();
}

TEST_CASE("the viewport list holds its viewports and the current one") {
    D3dRig rig;
    Device *device = rig.MakeDevice();
    Viewport *first = nullptr;
    Viewport *second = nullptr;
    REQUIRE(rig.d3d->CreateViewport(&first) == kDdOk);
    REQUIRE(rig.d3d->CreateViewport(&second) == kDdOk);
    CHECK(device->SetCurrentViewport(first) == kD3dErrInvalidCurrentViewport);
    CHECK(device->AddViewport(first) == kDdOk);
    CHECK(device->AddViewport(second) == kDdOk);
    CHECK(device->AddViewport(first) == ddraw4::kDdErrInvalidParams);
    CHECK(first->RefCount() == 2);
    CHECK(device->SetCurrentViewport(first) == kDdOk);
    CHECK(first->RefCount() == 3);

    Viewport *found = nullptr;
    CHECK(device->NextViewport(nullptr, kNextHead, &found) == kDdOk);
    CHECK(found == first);
    CHECK(device->NextViewport(first, kNextNext, &found) == kDdOk);
    CHECK(found == second);
    CHECK(device->NextViewport(second, kNextNext, &found) == ddraw4::kDdErrNotFound);
    CHECK(device->NextViewport(nullptr, kNextTail, &found) == kDdOk);
    CHECK(found == second);
    CHECK(device->GetCurrentViewport(&found) == kDdOk);
    CHECK(found == first);
    found->Release();

    CHECK(device->DeleteViewport(first) == kDdOk);
    CHECK(first->RefCount() == 1);
    CHECK(first->AttachedDevice() == nullptr);
    CHECK(device->GetCurrentViewport(&found) == kD3dErrNoCurrentViewport);
    CHECK(device->DeleteViewport(first) == ddraw4::kDdErrInvalidParams);
    device->Release(); // releases the second viewport's list reference
    CHECK(second->RefCount() == 1);
    CHECK(second->AttachedDevice() == nullptr);
    first->Release();
    second->Release();
}

TEST_CASE("GetCaps and EnumTextureFormats describe what the Gpu draws") {
    D3dRig rig;
    rig.gpu.capsResult = platform::GpuCaps{
        2048, {PixelFormat::kRgb565, PixelFormat::kArgb8888, PixelFormat::kIndexed8}, true};
    Device *device = rig.MakeDevice();
    DeviceDesc hardware;
    DeviceDesc software;
    CHECK(device->GetCaps(&hardware, &software) == kDdOk);
    CHECK(hardware.devCaps == 0x00082E51u);
    CHECK(hardware.maxTextureHeight == 2048u);
    CHECK(software.flags == 0u);
    CHECK(device->GetCaps(nullptr, nullptr) == ddraw4::kDdErrInvalidParams);

    std::vector<ddraw4::PixelFormatDesc> formats;
    CHECK(device->EnumTextureFormats(formats) == kDdOk);
    REQUIRE(formats.size() == 3);
    CHECK(formats[0].redMask == 0xF800u);
    CHECK(formats[1].alphaMask == 0xFF000000u);
    CHECK(formats[2].flags == (ddraw4::kDdpfRgb | ddraw4::kDdpfPaletteIndexed8));
    device->Release();
}

TEST_CASE("SetRenderTarget moves the device to another 3D surface") {
    D3dRig rig;
    Device *device = rig.MakeDevice();
    ddraw4::SurfaceDesc desc = ddraw4::test::plainDesc(256, 256, PixelFormat::kRgb565);
    desc.caps.caps = ddraw4::kDdscaps3dDevice | ddraw4::kDdscapsOffscreenPlain;
    ddraw4::Surface *target = rig.Create(desc);
    CHECK(device->SetRenderTarget(target, 0) == kDdOk);
    CHECK(target->RefCount() == 2);
    CHECK(rig.back->RefCount() == 1);
    ddraw4::Surface *got = nullptr;
    CHECK(device->GetRenderTarget(&got) == kDdOk);
    CHECK(got == target);
    got->Release();
    ddraw4::Surface *plain = rig.Create(ddraw4::test::plainDesc(8, 8, PixelFormat::kRgb565));
    CHECK(device->SetRenderTarget(plain, 0) == ddraw4::kDdErrInvalidCaps);
    plain->Release();
    device->Release();
    target->Release();
}

namespace {

// The window at 2560x1440 drawable pixels (1280x720 points on a two-pixel display), where the
// 800x600 frame is placed at 1920x1440: a render scale of 3.
void giveLargeWindow(D3dRig &rig) {
    rig.window.stateResult = platform::WindowState{platform::WindowMode::kWindowed, 1280, 720,
                                                   2560, 1440, true, false};
    platform::GpuCaps caps;
    caps.maxTextureSize = 8192;
    rig.gpu.capsResult = caps;
}

// One frame of combat as the game draws it: a scene with a triangle, the HUD written into a strip
// of the back buffer through a lock, and the flip.
void drawFrame(D3dRig &rig, Device &device) {
    REQUIRE(device.BeginScene() == kDdOk);
    REQUIRE(device.DrawPrimitive(kTriangleList, kFvfTlVertex, kTriangle, 3, 0) == kDdOk);
    REQUIRE(device.EndScene() == kDdOk);
    ddraw4::SurfaceDesc locked;
    const ddraw4::RectL hud{0, 560, 800, 600};
    REQUIRE(rig.back->Lock(&hud, locked, 0) == kDdOk);
    locked.surface[0] = 0x1F;
    REQUIRE(rig.back->Unlock() == kDdOk);
    REQUIRE(rig.front->Flip(nullptr, kDdflipWait) == kDdOk);
}

} // namespace

TEST_CASE("a scene in a window of more pixels than the frame draws into targets at its scale") {
    D3dRig rig;
    giveLargeWindow(rig);
    Device *device = rig.MakeDevice();
    Viewport *viewport = rig.MakeViewport(*device);
    giveTextures(rig); // the back buffer is texture 5 and the z-buffer 6, both at 1
    rig.gpu.backBufferResult = TextureId{9};
    CHECK(device->BeginScene() == kDdOk);
    // The display, the back buffer and its z-buffer are made again at 3; the back buffer's host
    // copy is uploaded into its new texture. The viewport stays in the game's pixels.
    CHECK(rig.gpu.log.Calls() ==
          Calls{"GetCaps()", "ConfigureScaledBackBuffer(800, 600, 0, 3)", "DestroyTexture(5)",
                "DestroyTexture(6)", "CreateTexture(800, 600, 0, \"scale\", 3)",
                "SetColorKey(7, \"none\")", "WriteTexture(7, 0, 0, 0, 800, 600, 1600)",
                "CreateTexture(800, 600, 5, \"scale\", 3)", "BeginScene(7, 7)",
                "SetViewport(0, 0, 800, 600)"});
    CHECK(rig.gpu.textureScales.at(9) == 3);
    CHECK(rig.gpu.textureScales.at(7) == 3);
    CHECK(rig.back->RenderScale() == 3);
    CHECK(rig.z->RenderScale() == 3);

    // A clear inside the scene: its rectangle in the game's pixels.
    rig.gpu.log.Clear();
    const std::vector<D3dRect> rects{{100, 100, 300, 200}};
    CHECK(viewport->Clear2(rects, kClearTarget, 0xFF102030, 1.0f, 0) == kDdOk);
    CHECK(rig.gpu.lastClear.area.x == 100);
    CHECK(rig.gpu.lastClear.area.width == 200);
    CHECK(rig.gpu.lastClear.area.height == 100);
    REQUIRE(device->EndScene() == kDdOk);

    // The flip copies the back buffer to the display, both at 3, in the game's pixels.
    rig.gpu.log.Clear();
    CHECK(rig.front->Flip(nullptr, kDdflipWait) == kDdOk);
    CHECK(rig.gpu.log.Count("Blit") == 1);
    CHECK(rig.gpu.lastBlit.source == TextureId{7});
    CHECK(rig.gpu.lastBlit.destination == TextureId{9});
    CHECK(rig.gpu.lastBlit.sourceRect.width == 800);
    CHECK(rig.gpu.lastBlit.destinationRect.height == 600);

    // The next scene in the same window makes nothing again.
    rig.gpu.log.Clear();
    REQUIRE(device->BeginScene() == kDdOk);
    CHECK(rig.gpu.log.Calls() == Calls{"BeginScene(7, 7)", "SetViewport(0, 0, 800, 600)"});
    REQUIRE(device->EndScene() == kDdOk);
    viewport->Release();
    device->Release();
}

TEST_CASE("a resize changes the targets' scale at the next scene's start, their content kept") {
    D3dRig rig;
    giveLargeWindow(rig);
    Device *device = rig.MakeDevice();
    giveTextures(rig);
    drawFrame(rig, *device); // at 3, as above: the back buffer is texture 7
    REQUIRE(device->BeginScene() == kDdOk);
    REQUIRE(device->DrawPrimitive(kTriangleList, kFvfTlVertex, kTriangle, 3, 0) == kDdOk);
    REQUIRE(device->EndScene() == kDdOk);

    // The window shrinks to 1600x1200 pixels, twice the frame. Nothing changes before a scene.
    rig.window.stateResult = platform::WindowState{platform::WindowMode::kWindowed, 800, 600,
                                                   1600, 1200, true, false};
    rig.gpu.log.Clear();
    REQUIRE(rig.front->Flip(nullptr, kDdflipWait) == kDdOk);
    CHECK(rig.gpu.log.Count("ConfigureScaledBackBuffer") == 0);
    CHECK(rig.back->RenderScale() == 3);

    // At the next scene: the drawn back buffer is read back, then made again at 2.
    rig.gpu.backBufferResult = TextureId{11};
    rig.gpu.createTextureResult = TextureId{12};
    rig.gpu.log.Clear();
    REQUIRE(device->BeginScene() == kDdOk);
    CHECK(rig.gpu.log.Calls() ==
          Calls{"ConfigureScaledBackBuffer(800, 600, 0, 2)",
                "ReadTexture(7, 0, 0, 0, 800, 600, 1600)", "DestroyTexture(7)",
                "DestroyTexture(7)", "CreateTexture(800, 600, 0, \"scale\", 2)",
                "SetColorKey(12, \"none\")", "WriteTexture(12, 0, 0, 0, 800, 600, 1600)",
                "CreateTexture(800, 600, 5, \"scale\", 2)", "BeginScene(12, 12)"});
    CHECK(rig.back->RenderScale() == 2);
    CHECK(rig.z->RenderScale() == 2);
    REQUIRE(device->EndScene() == kDdOk);
    device->Release();
}

TEST_CASE("frames drawn only on the host stay at the game's own size in a large window") {
    D3dRig rig;
    giveLargeWindow(rig);
    for (int frame = 0; frame < 3; ++frame) {
        ddraw4::test::writePixels16(*rig.back, std::vector<std::uint16_t>(800 * 600, 0x7E0));
        REQUIRE(rig.front->Flip(nullptr, kDdflipWait) == kDdOk);
    }
    CHECK(rig.gpu.log.Count("ConfigureScaledBackBuffer") == 0);
    CHECK(rig.gpu.log.Count("GetCaps") == 0);
    for (const auto &[texture, scale] : rig.gpu.textureScales) {
        CHECK(scale == 1);
    }
    CHECK(rig.dd->DisplayScale() == 1);
    CHECK(rig.back->RenderScale() == 1);
}

TEST_CASE("uploads per frame are the same at render scale 3 as at the game's own size") {
    D3dRig native;
    Device *nativeDevice = native.MakeDevice();
    giveTextures(native);
    D3dRig scaled;
    giveLargeWindow(scaled);
    Device *scaledDevice = scaled.MakeDevice();
    giveTextures(scaled);
    // The first frame makes the textures; from the second on, each frame uploads the HUD strip
    // once (at the flip), at either scale.
    drawFrame(native, *nativeDevice);
    drawFrame(scaled, *scaledDevice);
    CHECK(scaled.back->RenderScale() == 3);
    CHECK(native.back->RenderScale() == 1);
    for (int frame = 0; frame < 3; ++frame) {
        const std::uint64_t nativeBefore = native.dd->Counted().uploads;
        const std::uint64_t scaledBefore = scaled.dd->Counted().uploads;
        drawFrame(native, *nativeDevice);
        drawFrame(scaled, *scaledDevice);
        CHECK(native.dd->Counted().uploads - nativeBefore == 1);
        CHECK(scaled.dd->Counted().uploads - scaledBefore == 1);
    }
    nativeDevice->Release();
    scaledDevice->Release();
}

TEST_CASE("with the render scale option at 1 a scene in a large window stays at 1") {
    D3dRig rig;
    giveLargeWindow(rig);
    Device *device = rig.MakeDevice();
    Viewport *viewport = rig.MakeViewport(*device);
    giveTextures(rig);
    ddraw4::DirectDraw::SetRenderScaleOption(1);
    CHECK(device->BeginScene() == kDdOk);
    CHECK(rig.gpu.log.Calls() == Calls{"BeginScene(5, 6)", "SetViewport(0, 0, 800, 600)"});
    REQUIRE(device->EndScene() == kDdOk);
    ddraw4::DirectDraw::SetRenderScaleOption(0);
    viewport->Release();
    device->Release();
}

namespace {

constexpr std::uint32_t kPointList = 1;
constexpr std::uint32_t kLineList = 2;
constexpr std::uint32_t kLineStrip = 3;

// A scene at the window's scale, the device's state as the game leaves it (culling on), with the
// log cleared: `scale` 3 in the large window, 1 at the game's own size.
Device *sceneAt(D3dRig &rig, std::uint8_t scale) {
    if (scale == 3) {
        giveLargeWindow(rig);
    }
    Device *device = rig.MakeDevice();
    giveTextures(rig);
    REQUIRE(device->BeginScene() == kDdOk);
    REQUIRE(rig.back->RenderScale() == scale);
    rig.gpu.log.Clear();
    return device;
}

} // namespace

TEST_CASE("points and lines are drawn as triangles one logical pixel wide at render scale 3") {
    D3dRig rig;
    Device *device = sceneAt(rig, 3);
    CHECK(device->DrawPrimitive(kPointList, kFvfTlVertex, kTriangle, 3, 0) == kDdOk);
    // Three points, two triangles each, in a pipeline that culls nothing.
    CHECK(rig.gpu.lastPipelineDesc.primitive == platform::PrimitiveType::kTriangleList);
    CHECK(rig.gpu.lastPipelineDesc.cull == platform::CullMode::kNone);
    CHECK(rig.gpu.lastDraw.vertexCount == 18);
    CHECK(rig.gpu.log.Count("WriteBuffer") == 1);

    CHECK(device->DrawPrimitive(kLineList, kFvfTlVertex, kTriangle, 2, 0) == kDdOk);
    CHECK(rig.gpu.lastDraw.vertexCount == 6);
    CHECK(device->DrawPrimitive(kLineStrip, kFvfTlVertex, kTriangle, 4, 0) == kDdOk);
    CHECK(rig.gpu.lastDraw.vertexCount == 18);
    const std::uint16_t indices[2] = {0, 3};
    CHECK(device->DrawIndexedPrimitive(kLineList, kFvfTlVertex, kTriangle, 4, indices, 2, 0) ==
          kDdOk);
    CHECK(rig.gpu.lastDraw.vertexCount == 6);
    CHECK_FALSE(rig.gpu.lastDraw.indices.Valid()); // widened vertices are drawn in order

    // Triangles are drawn as the game sends them.
    CHECK(device->DrawPrimitive(kTriangleList, kFvfTlVertex, kTriangle, 3, 0) == kDdOk);
    CHECK(rig.gpu.lastPipelineDesc.primitive == platform::PrimitiveType::kTriangleList);
    CHECK(rig.gpu.lastDraw.vertexCount == 3);
    REQUIRE(device->EndScene() == kDdOk);
    device->Release();
}

TEST_CASE("points and lines are drawn as they are at render scale 1") {
    D3dRig rig;
    Device *device = sceneAt(rig, 1);
    CHECK(device->DrawPrimitive(kPointList, kFvfTlVertex, kTriangle, 3, 0) == kDdOk);
    CHECK(rig.gpu.lastPipelineDesc.primitive == platform::PrimitiveType::kPointList);
    CHECK(rig.gpu.lastDraw.vertexCount == 3);
    CHECK(device->DrawPrimitive(kLineList, kFvfTlVertex, kTriangle, 2, 0) == kDdOk);
    CHECK(rig.gpu.lastPipelineDesc.primitive == platform::PrimitiveType::kLineList);
    CHECK(rig.gpu.lastDraw.vertexCount == 2);
    REQUIRE(device->EndScene() == kDdOk);
    device->Release();
}

} // namespace sfcop::hle::d3d3
