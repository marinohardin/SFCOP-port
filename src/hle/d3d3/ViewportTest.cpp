#include "hle/d3d3/D3d3TestRig.h"

namespace sfcop::hle::d3d3 {

using test::D3dRig;

TEST_CASE("a viewport has no data until it is set") {
    D3dRig rig;
    Viewport *viewport = nullptr;
    REQUIRE(rig.d3d->CreateViewport(&viewport) == kDdOk);
    ViewportDesc desc;
    CHECK(viewport->GetViewport2(desc) == kD3dErrViewportDataNotSet);
    ViewportV1 v1;
    CHECK(viewport->GetViewport(v1) == kD3dErrViewportDataNotSet);
    desc.clipWidth = 0.0f;
    CHECK(viewport->SetViewport2(desc) == ddraw4::kDdErrInvalidParams);
    CHECK(viewport->Initialize() == ddraw4::kDdErrAlreadyInitialized);
    viewport->Release();
}

TEST_CASE("SetViewport2 is read back as given") {
    D3dRig rig;
    Viewport *viewport = nullptr;
    REQUIRE(rig.d3d->CreateViewport(&viewport) == kDdOk);
    ViewportDesc desc;
    desc.x = 16;
    desc.y = 8;
    desc.width = 320;
    desc.height = 240;
    desc.clipX = -2.0f;
    desc.clipY = 1.5f;
    desc.clipWidth = 4.0f;
    desc.clipHeight = 3.0f;
    desc.minZ = 0.25f;
    desc.maxZ = 0.75f;
    REQUIRE(viewport->SetViewport2(desc) == kDdOk);
    ViewportDesc out;
    CHECK(viewport->GetViewport2(out) == kDdOk);
    CHECK(out.x == 16u);
    CHECK(out.y == 8u);
    CHECK(out.width == 320u);
    CHECK(out.height == 240u);
    CHECK(out.clipX == -2.0f);
    CHECK(out.clipY == 1.5f);
    CHECK(out.clipWidth == 4.0f);
    CHECK(out.clipHeight == 3.0f);
    CHECK(out.minZ == 0.25f);
    CHECK(out.maxZ == 0.75f);

    ViewportV1 v1;
    CHECK(viewport->GetViewport(v1) == kDdOk);
    CHECK(v1.maxX == 2.0f);
    CHECK(v1.maxY == 1.5f);
    CHECK(v1.scaleX == 80.0f);
    CHECK(v1.scaleY == 80.0f);
    viewport->Release();
}

TEST_CASE("the version 1 extents become a clip volume centred on the origin") {
    D3dRig rig;
    Viewport *viewport = nullptr;
    REQUIRE(rig.d3d->CreateViewport(&viewport) == kDdOk);
    ViewportV1 v1;
    v1.width = 640;
    v1.height = 480;
    v1.maxX = 1.0f;
    v1.maxY = 0.75f;
    REQUIRE(viewport->SetViewport(v1) == kDdOk);
    ViewportDesc out;
    REQUIRE(viewport->GetViewport2(out) == kDdOk);
    CHECK(out.clipX == -1.0f);
    CHECK(out.clipY == 0.75f);
    CHECK(out.clipWidth == 2.0f);
    CHECK(out.clipHeight == 1.5f);
    v1.maxY = 0.0f;
    CHECK(viewport->SetViewport(v1) == ddraw4::kDdErrInvalidParams);
    viewport->Release();
}

TEST_CASE("a viewport clears only through the device it was added to") {
    D3dRig rig;
    Viewport *viewport = nullptr;
    REQUIRE(rig.d3d->CreateViewport(&viewport) == kDdOk);
    CHECK(viewport->Clear({{0, 0, 8, 8}}, kClearTarget) == kD3dErrViewportHasNoDevice);
    Device *device = rig.MakeDevice();
    ViewportDesc desc;
    desc.width = 800;
    desc.height = 600;
    REQUIRE(viewport->SetViewport2(desc) == kDdOk);
    REQUIRE(device->AddViewport(viewport) == kDdOk);
    CHECK(viewport->AttachedDevice() == device);
    rig.gpu.createTextureResult = platform::TextureId{5};
    REQUIRE(rig.back->GpuTexture().Ok()); // the texture is made at the first Gpu use
    CHECK(viewport->Clear({{0, 0, 8, 8}}, kClearTarget) == kDdOk);
    CHECK(rig.gpu.log.Calls().back() == "Fill(5, 0, 0, 8, 8, 0)"); // Clear is opaque black
    device->Release();
    viewport->Release();
}

TEST_CASE("the background material is kept") {
    D3dRig rig;
    Viewport *viewport = nullptr;
    REQUIRE(rig.d3d->CreateViewport(&viewport) == kDdOk);
    std::uint32_t material = 9;
    bool valid = true;
    CHECK(viewport->GetBackground(material, valid) == kDdOk);
    CHECK(material == 0u);
    CHECK_FALSE(valid);
    CHECK(viewport->SetBackground(3) == kDdOk);
    CHECK(viewport->GetBackground(material, valid) == kDdOk);
    CHECK(material == 3u);
    CHECK(valid);
    viewport->Release();
}

TEST_CASE("Clear2 with no rectangles clears the whole viewport") {
    D3dRig rig;
    Device *device = rig.MakeDevice();
    Viewport *viewport = nullptr;
    REQUIRE(rig.d3d->CreateViewport(&viewport) == kDdOk);
    ViewportDesc desc;
    desc.x = 100;
    desc.y = 50;
    desc.width = 200;
    desc.height = 100;
    REQUIRE(viewport->SetViewport2(desc) == kDdOk);
    REQUIRE(device->AddViewport(viewport) == kDdOk);
    REQUIRE(device->SetCurrentViewport(viewport) == kDdOk);
    rig.gpu.createTextureResult = platform::TextureId{5};
    REQUIRE(rig.back->GpuTexture().Ok()); // the texture is made at the first Gpu use
    CHECK(viewport->Clear2({}, kClearTarget, 0xFF0000FF, 1.0f, 0) == kDdOk);
    CHECK(rig.gpu.log.Calls().back() == "Fill(5, 100, 50, 200, 100, 31)");
    // Inside a scene the Gpu records one Clear over the viewport's rectangle.
    rig.gpu.createTextureResult = platform::TextureId{6};
    ddraw4::BltFx fx;
    fx.fillColor = 0xFFFF;
    REQUIRE(rig.z->Blt(nullptr, nullptr, nullptr, ddraw4::kDdbltColorFill, &fx) == kDdOk);
    REQUIRE(device->BeginScene() == kDdOk);
    rig.gpu.log.Clear();
    CHECK(viewport->Clear2({}, kClearTarget, 0xFF0000FF, 1.0f, 0) == kDdOk);
    CHECK(rig.gpu.log.Count("Clear") == 1);
    CHECK(rig.gpu.lastClear.area.x == 100);
    CHECK(rig.gpu.lastClear.area.y == 50);
    CHECK(rig.gpu.lastClear.area.width == 200);
    CHECK(rig.gpu.lastClear.area.height == 100);
    REQUIRE(device->EndScene() == kDdOk);
    device->Release();
    viewport->Release();
}

} // namespace sfcop::hle::d3d3
