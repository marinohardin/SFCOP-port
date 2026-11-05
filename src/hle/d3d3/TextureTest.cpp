#include "hle/d3d3/D3d3TestRig.h"

namespace sfcop::hle::d3d3 {

using platform::PixelFormat;
using test::D3dRig;

namespace {

ddraw4::Surface *makeTextureSurface(D3dRig &rig, std::uint32_t size, PixelFormat format) {
    ddraw4::SurfaceDesc desc = ddraw4::test::plainDesc(size, size, format);
    desc.caps.caps = ddraw4::kDdscapsTexture;
    return rig.Create(desc);
}

} // namespace

TEST_CASE("a texture holds its surface") {
    D3dRig rig;
    ddraw4::Surface *surface = makeTextureSurface(rig, 16, PixelFormat::kRgb565);
    Texture *texture = new Texture(*surface);
    CHECK(surface->RefCount() == 2);
    CHECK(&texture->Surface() == surface);
    surface->Release();
    CHECK_FALSE(ddraw4::test::wasDestroyed(surface));
    texture->Release();
    CHECK(ddraw4::test::wasDestroyed(surface));
}

TEST_CASE("a texture handle names its surface, the same through every view") {
    D3dRig rig;
    ddraw4::Surface *surface = makeTextureSurface(rig, 16, PixelFormat::kRgb565);
    Texture *texture = new Texture(*surface);
    Texture *second = new Texture(*surface);
    std::uint32_t first = 0;
    std::uint32_t again = 0;
    std::uint32_t other = 0;
    CHECK(texture->GetHandle(nullptr, first) == kDdOk);
    CHECK(first != 0u);
    CHECK(texture->GetHandle(nullptr, again) == kDdOk);
    CHECK(again == first);
    CHECK(second->GetHandle(nullptr, other) == kDdOk);
    CHECK(other == first);
    Texture *named = Texture::FromHandle(first);
    REQUIRE(named != nullptr);
    CHECK(&named->Surface() == surface);
    named->Release();
    CHECK(Texture::FromHandle(0) == nullptr);
    CHECK(Texture::FromHandle(first + 100) == nullptr);
    texture->Release();
    second->Release();
    surface->Release();
}

TEST_CASE("a handle outlives its view while the surface lives") {
    D3dRig rig;
    Device *device = rig.MakeDevice();
    Viewport *viewport = rig.MakeViewport(*device);
    ddraw4::Surface *surface = makeTextureSurface(rig, 16, PixelFormat::kRgb565);
    Texture *view = new Texture(*surface); // as QueryInterface for IDirect3DTexture2 makes it
    std::uint32_t handle = 0;
    REQUIRE(view->GetHandle(device, handle) == kDdOk);
    view->Release(); // the game lets go of the view and keeps the surface
    CHECK_FALSE(ddraw4::test::wasDestroyed(surface));
    CHECK(surface->RefCount() == 1);

    // Back buffer 5, z-buffer 6, the texture's own 7.
    rig.gpu.createTextureResult = platform::TextureId{5};
    ddraw4::SurfaceDesc locked;
    REQUIRE(rig.back->Lock(nullptr, locked, 0) == kDdOk);
    REQUIRE(rig.back->Unlock() == kDdOk);
    rig.gpu.createTextureResult = platform::TextureId{6};
    ddraw4::BltFx fx;
    fx.fillColor = 0xFFFF;
    REQUIRE(rig.z->Blt(nullptr, nullptr, nullptr, ddraw4::kDdbltColorFill, &fx) == kDdOk);
    rig.gpu.createTextureResult = platform::TextureId{7};

    CHECK(device->SetRenderState(kRsTextureHandle, handle) == kDdOk);
    REQUIRE(device->BeginScene() == kDdOk);
    const test::TlVertex triangle[3] = {
        {10.0f, 10.0f, 0.5f, 1.0f, 0xFFFF0000, 0xFF000000, 0.0f, 0.0f},
        {100.0f, 10.0f, 0.5f, 1.0f, 0xFF00FF00, 0xFF000000, 1.0f, 0.0f},
        {10.0f, 100.0f, 0.5f, 1.0f, 0xFF0000FF, 0xFF000000, 0.0f, 1.0f},
    };
    CHECK(device->DrawPrimitive(4, kFvfTlVertex, triangle, 3, 0) == kDdOk);
    CHECK(rig.gpu.lastDraw.state.textures[0].value == 7);
    REQUIRE(device->EndScene() == kDdOk);

    CHECK(device->SetRenderState(kRsTextureHandle, 0) == kDdOk); // the stage lets go
    surface->Release();
    CHECK(ddraw4::test::wasDestroyed(surface));
    Texture *gone = Texture::FromHandle(handle);
    CHECK(gone == nullptr);
    if (gone != nullptr) {
        gone->Release();
    }
    viewport->Release();
    device->Release();
}

TEST_CASE("Load copies the pixels and the colour key of a matching texture") {
    D3dRig rig;
    rig.gpu.createTextureResult = platform::TextureId{3};
    ddraw4::Surface *fromSurface = makeTextureSurface(rig, 4, PixelFormat::kRgb565);
    ddraw4::Surface *toSurface = makeTextureSurface(rig, 4, PixelFormat::kRgb565);
    ddraw4::BltFx fx;
    fx.fillColor = 0x1234;
    REQUIRE(fromSurface->Blt(nullptr, nullptr, nullptr, ddraw4::kDdbltColorFill, &fx) == kDdOk);
    ddraw4::ColorKey key{0xF81F, 0xF81F};
    REQUIRE(fromSurface->SetColorKey(ddraw4::kDdckeySrcBlt, &key) == kDdOk);
    Texture *from = new Texture(*fromSurface);
    Texture *to = new Texture(*toSurface);
    fromSurface->Release();
    toSurface->Release();

    rig.gpu.createTextureResult = platform::TextureId{4};
    rig.gpu.log.Clear();
    CHECK(to->Load(from) == kDdOk);
    // Both host copies are current: the copy is made there, and the texture gets it at its use.
    CHECK(rig.gpu.log.Calls().empty());
    CHECK(ddraw4::test::readPixels16(*toSurface) == std::vector<std::uint16_t>(16, 0x1234));
    CHECK(toSurface->GpuTexture().Ok());
    CHECK(rig.gpu.log.Calls() == test::Calls{"CreateTexture(4, 4, 0)", "SetColorKey(4, 63519)",
                                             "WriteTexture(4, 0, 0, 0, 4, 4, 8)"});
    ddraw4::ColorKey got;
    CHECK(toSurface->GetColorKey(ddraw4::kDdckeySrcBlt, got) == kDdOk);
    CHECK(got.low == 0xF81Fu);
    CHECK(to->Load(nullptr) == ddraw4::kDdErrInvalidParams);
    from->Release();
    to->Release();
}

TEST_CASE("Load copies the pixels on the Gpu from a texture the Gpu drew") {
    D3dRig rig;
    rig.gpu.createTextureResult = platform::TextureId{3};
    ddraw4::Surface *fromSurface = makeTextureSurface(rig, 4, PixelFormat::kRgb565);
    ddraw4::Surface *toSurface = makeTextureSurface(rig, 4, PixelFormat::kRgb565);
    ddraw4::ColorKey key{0xF81F, 0xF81F};
    REQUIRE(fromSurface->SetColorKey(ddraw4::kDdckeySrcBlt, &key) == kDdOk);
    REQUIRE(fromSurface->GpuTexture().Ok());
    fromSurface->MarkGpuWritten(); // a scene drew into it: its host copy is stale
    Texture *from = new Texture(*fromSurface);
    Texture *to = new Texture(*toSurface);
    fromSurface->Release();
    toSurface->Release();

    rig.gpu.createTextureResult = platform::TextureId{4};
    rig.gpu.log.Clear();
    CHECK(to->Load(from) == kDdOk);
    CHECK(rig.gpu.log.Calls() == test::Calls{"CreateTexture(4, 4, 0)", "SetColorKey(4, 63519)",
                                             "WriteTexture(4, 0, 0, 0, 4, 4, 8)",
                                             "Blit(3, 4, false)"});
    from->Release();
    to->Release();
}

TEST_CASE("Load refuses a texture of another size or format") {
    D3dRig rig;
    ddraw4::Surface *small = makeTextureSurface(rig, 4, PixelFormat::kRgb565);
    ddraw4::Surface *large = makeTextureSurface(rig, 8, PixelFormat::kRgb565);
    ddraw4::Surface *other = makeTextureSurface(rig, 4, PixelFormat::kArgb1555);
    Texture *a = new Texture(*small);
    Texture *b = new Texture(*large);
    Texture *c = new Texture(*other);
    small->Release();
    large->Release();
    other->Release();
    CHECK(a->Load(b) == ddraw4::kDdErrInvalidParams);
    CHECK(a->Load(c) == ddraw4::kDdErrInvalidParams);
    CHECK(a->PaletteChanged(0, 256) == kDdOk);
    a->Release();
    b->Release();
    c->Release();
}

} // namespace sfcop::hle::d3d3
