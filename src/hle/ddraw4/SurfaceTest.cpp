#include "hle/ddraw4/Clipper.h"
#include "hle/ddraw4/Ddraw4TestRig.h"
#include "hle/ddraw4/Palette.h"

#include "fixes/fixes.h"

#include <algorithm>
#include <array>
#include <utility>

namespace sfcop::hle::ddraw4 {

using platform::Error;
using platform::PixelFormat;
using platform::TextureId;
using test::Calls;
using test::Rig;

namespace {

constexpr std::uint32_t kDdlockWait = 0x1;
constexpr std::uint32_t kDdbltWait = 0x01000000;
constexpr std::uint32_t kDdflipWait = 0x1;

// Gives a surface its texture, with the number `id`, as a first use by the Gpu does.
void giveTexture(Rig &rig, Surface &surface, std::uint32_t id) {
    rig.gpu.createTextureResult = TextureId{id};
    REQUIRE(surface.GpuTexture().Ok());
}

// Records that the Gpu drew into the surface (a Direct3D scene), so that its host copy is stale
// and Blt and fills run on the Gpu.
void drawnByGpu(Surface &surface) {
    surface.MarkGpuWritten();
}

// Locks `rect` of a surface and unlocks it: one of the game's 2D draws.
void touch(Surface &surface, const RectL &rect) {
    SurfaceDesc locked;
    REQUIRE(surface.Lock(&rect, locked, 0) == kDdOk);
    REQUIRE(surface.Unlock() == kDdOk);
}

} // namespace

TEST_CASE("Lock hands out the host pixels; the texture is made and filled at its first use") {
    Rig rig;
    Surface *surface = rig.Create(test::plainDesc(4, 2, PixelFormat::kRgb565));
    SurfaceDesc locked;
    CHECK(surface->Lock(nullptr, locked, kDdlockWait) == kDdOk);
    REQUIRE(locked.surface != nullptr);
    CHECK((locked.flags & kDdsdLpSurface) != 0);
    CHECK(locked.width == 4);
    CHECK(locked.height == 2);
    CHECK(locked.pitch == 8);
    CHECK(locked.pixelFormat.bitCount == 16);
    CHECK(locked.surface[0] == 0); // a new surface is black
    locked.surface[0] = 0x1F;

    SurfaceDesc again;
    CHECK(surface->Lock(nullptr, again, 0) == kDdErrSurfaceBusy);
    CHECK(surface->Unlock() == kDdOk);
    CHECK(rig.gpu.log.Calls().empty()); // nothing reaches the Gpu before it uses the texture
    CHECK(surface->GpuTexture().Ok());
    CHECK(rig.gpu.log.Calls() == Calls{"CreateTexture(4, 2, 0)", "SetColorKey(2, \"none\")",
                                       "WriteTexture(2, 0, 0, 0, 4, 2, 8)"});
    CHECK_FALSE(rig.gpu.lastTextureDesc.renderTarget);
    CHECK(surface->Unlock() == kDdErrNotLocked);
    surface->Release();
    CHECK(rig.gpu.log.Calls().back() == "DestroyTexture(2)");
}

TEST_CASE("Lock of a rectangle points at its first pixel and only it is uploaded") {
    Rig rig;
    Surface *surface = rig.Create(test::plainDesc(4, 2, PixelFormat::kRgb565));
    giveTexture(rig, *surface, 2);
    rig.gpu.log.Clear();
    SurfaceDesc whole;
    REQUIRE(surface->Lock(nullptr, whole, 0) == kDdOk);
    std::uint8_t *base = whole.surface;
    REQUIRE(surface->Unlock() == kDdOk);
    REQUIRE(surface->GpuTexture().Ok());
    rig.gpu.log.Clear();

    const RectL rect{1, 1, 3, 2};
    SurfaceDesc locked;
    CHECK(surface->Lock(&rect, locked, 0) == kDdOk);
    CHECK(locked.surface == base + 8 + 2);
    CHECK(surface->Unlock() == kDdOk);
    CHECK(surface->GpuTexture().Ok());
    CHECK(rig.gpu.log.Calls() == Calls{"WriteTexture(2, 0, 1, 1, 2, 1, 8)"});

    const RectL outside{2, 0, 5, 1};
    CHECK(surface->Lock(&outside, locked, 0) == kDdErrInvalidRect);
    surface->Release();
}

TEST_CASE("a read-only lock uploads nothing") {
    Rig rig;
    Surface *surface = rig.Create(test::plainDesc(4, 2, PixelFormat::kRgb565));
    giveTexture(rig, *surface, 2);
    rig.gpu.log.Clear();
    SurfaceDesc locked;
    CHECK(surface->Lock(nullptr, locked, kDdlockReadOnly) == kDdOk);
    CHECK(surface->Unlock() == kDdOk);
    CHECK(rig.gpu.log.Calls().empty());
    surface->Release();
}

TEST_CASE("a fill of a current host copy fills it there, uploaded at the next use") {
    Rig rig;
    Surface *surface = rig.Create(test::plainDesc(4, 2, PixelFormat::kRgb565));
    BltFx fx;
    fx.fillColor = 0x07E0;
    CHECK(surface->Blt(nullptr, nullptr, nullptr, kDdbltColorFill | kDdbltWait, &fx) == kDdOk);
    CHECK(rig.gpu.log.Calls().empty());
    CHECK(test::readPixels16(*surface) == std::vector<std::uint16_t>(8, 0x07E0));
    CHECK(rig.gpu.log.Calls().empty());
    CHECK(surface->GpuTexture().Ok());
    CHECK(rig.gpu.log.Calls() == Calls{"CreateTexture(4, 2, 0)", "SetColorKey(2, \"none\")",
                                       "WriteTexture(2, 0, 0, 0, 4, 2, 8)"});
    surface->Release();
}

TEST_CASE("Lock after the Gpu drew reads the texture back") {
    Rig rig;
    Surface *source = rig.Create(test::plainDesc(4, 2, PixelFormat::kRgb565));
    Surface *surface = rig.Create(test::plainDesc(4, 2, PixelFormat::kRgb565));
    giveTexture(rig, *source, 3);
    giveTexture(rig, *surface, 2);
    drawnByGpu(*surface);
    CHECK(surface->Blt(nullptr, source, nullptr, kDdbltWait, nullptr) == kDdOk);
    rig.gpu.log.Clear();
    rig.gpu.readValue = 0xAB;
    SurfaceDesc locked;
    CHECK(surface->Lock(nullptr, locked, 0) == kDdOk);
    CHECK(rig.gpu.log.Calls() == Calls{"ReadTexture(2, 0, 0, 0, 4, 2, 8)"});
    CHECK(locked.surface[0] == 0xAB);
    CHECK(locked.surface[15] == 0xAB);
    CHECK(surface->Unlock() == kDdOk);
    source->Release();
    surface->Release();
}

TEST_CASE("Blt fills with DDBLT_COLORFILL and the BLACKNESS and WHITENESS operations") {
    Rig rig;
    Surface *surface = rig.Create(test::plainDesc(8, 8, PixelFormat::kRgb565));
    giveTexture(rig, *surface, 2);
    drawnByGpu(*surface); // on the Gpu: the host copy is stale
    rig.gpu.log.Clear();
    const RectL area{2, 2, 6, 4};
    BltFx fx;
    fx.fillColor = 0x12345678; // only the format's bits are kept
    CHECK(surface->Blt(&area, nullptr, nullptr, kDdbltColorFill, &fx) == kDdOk);
    fx.rop = kRopWhiteness;
    CHECK(surface->Blt(nullptr, nullptr, nullptr, kDdbltRop, &fx) == kDdOk);
    fx.rop = kRopBlackness;
    CHECK(surface->Blt(nullptr, nullptr, nullptr, kDdbltRop, &fx) == kDdOk);
    CHECK(rig.gpu.log.Calls() == Calls{"Fill(2, 2, 2, 4, 2, 22136)", "Fill(2, 0, 0, 8, 8, 65535)",
                                       "Fill(2, 0, 0, 8, 8, 0)"});
    CHECK(surface->Blt(nullptr, nullptr, nullptr, kDdbltColorFill, nullptr) == kDdErrInvalidParams);
    surface->Release();
}

TEST_CASE("Blt into a surface the Gpu drew copies on the Gpu with the source's colour key") {
    Rig rig;
    Surface *source = rig.Create(test::plainDesc(8, 8, PixelFormat::kRgb565));
    Surface *destination = rig.Create(test::plainDesc(16, 16, PixelFormat::kRgb565));
    const ColorKey magenta{0xF81F, 0xF81F};
    CHECK(source->SetColorKey(kDdckeySrcBlt, &magenta) == kDdOk);
    giveTexture(rig, *source, 3);
    giveTexture(rig, *destination, 4);
    drawnByGpu(*destination);
    rig.gpu.log.Clear();

    const RectL to{4, 4, 12, 12};
    CHECK(destination->Blt(&to, source, nullptr, kDdbltKeySrc | kDdbltWait, nullptr) == kDdOk);
    CHECK(rig.gpu.log.Calls() == Calls{"Blit(3, 4, true)"});
    CHECK(rig.gpu.lastBlit.sourceRect.width == 8);
    CHECK(rig.gpu.lastBlit.destinationRect.x == 4);
    CHECK(rig.gpu.lastBlit.destinationRect.width == 8);

    rig.gpu.log.Clear();
    const RectL stretched{0, 0, 16, 16};
    CHECK(destination->Blt(&stretched, source, nullptr, kDdbltWait, nullptr) == kDdOk);
    CHECK(rig.gpu.log.Calls() == Calls{"Blit(3, 4, false)"});
    CHECK(rig.gpu.lastBlit.destinationRect.width == 16);
    source->Release();
    destination->Release();
}

TEST_CASE("Blt with DDBLT_KEYSRCOVERRIDE keys with DDBLTFX's key for that blit only") {
    Rig rig;
    Surface *source = rig.Create(test::plainDesc(8, 8, PixelFormat::kRgb565));
    Surface *destination = rig.Create(test::plainDesc(8, 8, PixelFormat::kRgb565));
    giveTexture(rig, *source, 3);
    giveTexture(rig, *destination, 4);
    drawnByGpu(*destination);
    rig.gpu.log.Clear();
    BltFx fx;
    fx.srcKey = ColorKey{0x001F, 0x001F};
    CHECK(destination->Blt(nullptr, source, nullptr, kDdbltKeySrcOverride, &fx) == kDdOk);
    CHECK(rig.gpu.log.Calls() ==
          Calls{"SetColorKey(3, 31)", "Blit(3, 4, true)", "SetColorKey(3, \"none\")"});
    source->Release();
    destination->Release();
}

TEST_CASE("Blt refuses destination keys, other raster operations and effects") {
    Rig rig;
    Surface *source = rig.Create(test::plainDesc(8, 8, PixelFormat::kRgb565));
    Surface *destination = rig.Create(test::plainDesc(8, 8, PixelFormat::kRgb565));
    BltFx fx;
    fx.rop = 0x00EE0086; // SRCPAINT
    CHECK(destination->Blt(nullptr, source, nullptr, kDdbltRop, &fx) == kDdErrUnsupported);
    CHECK(destination->Blt(nullptr, source, nullptr, kDdbltKeyDest, nullptr) == kDdErrUnsupported);
    fx.ddfx = 0x2; // DDBLTFX_MIRRORLEFTRIGHT
    CHECK(destination->Blt(nullptr, source, nullptr, kDdbltDdfx, &fx) == kDdErrUnsupported);
    CHECK(destination->Blt(nullptr, nullptr, nullptr, 0, nullptr) == kDdErrInvalidParams);
    const RectL outside{0, 0, 9, 8};
    CHECK(destination->Blt(nullptr, source, &outside, 0, nullptr) == kDdErrInvalidRect);
    CHECK(rig.gpu.log.Count("Blit") == 0);
    source->Release();
    destination->Release();
}

TEST_CASE("Blt clips to the surface only when a clipper is attached") {
    Rig rig;
    Surface *source = rig.Create(test::plainDesc(4, 4, PixelFormat::kRgb565));
    Surface *destination = rig.Create(test::plainDesc(4, 4, PixelFormat::kRgb565));
    giveTexture(rig, *source, 3);
    giveTexture(rig, *destination, 4);
    drawnByGpu(*destination);
    const RectL hanging{-2, 0, 2, 4};
    CHECK(destination->Blt(&hanging, source, nullptr, 0, nullptr) == kDdErrInvalidRect);

    Clipper *clipper = nullptr;
    REQUIRE(rig.dd->CreateClipper(0, &clipper) == kDdOk);
    CHECK(destination->SetClipper(clipper) == kDdOk);
    CHECK(destination->Blt(&hanging, source, nullptr, 0, nullptr) == kDdOk);
    CHECK(rig.gpu.lastBlit.sourceRect.x == 2);
    CHECK(rig.gpu.lastBlit.sourceRect.width == 2);
    CHECK(rig.gpu.lastBlit.destinationRect.x == 0);
    CHECK(rig.gpu.lastBlit.destinationRect.width == 2);
    clipper->Release();
    source->Release();
    destination->Release();
}

TEST_CASE("Blt within one surface copies on the host, overlapping or not") {
    Rig rig;
    Surface *surface = rig.Create(test::plainDesc(4, 1, PixelFormat::kRgb565));
    test::writePixels16(*surface, {1, 2, 3, 4});
    rig.gpu.log.Clear();
    const RectL from{0, 0, 3, 1};
    const RectL to{1, 0, 4, 1};
    CHECK(surface->Blt(&to, surface, &from, kDdbltWait, nullptr) == kDdOk);
    CHECK(rig.gpu.log.Calls().empty());
    CHECK(test::readPixels16(*surface) == std::vector<std::uint16_t>{1, 1, 2, 3});
    surface->Release();
}

TEST_CASE("BltFast copies with the source key on the host when the Gpu has no texture") {
    Rig rig;
    rig.gpu.createTextureResult = Error::kOutOfResources;
    Surface *source = rig.Create(test::plainDesc(2, 1, PixelFormat::kRgb565));
    Surface *destination = rig.Create(test::plainDesc(3, 1, PixelFormat::kRgb565));
    test::writePixels16(*source, {7, 9});
    test::writePixels16(*destination, {5, 5, 5});
    const ColorKey key{9, 9};
    CHECK(source->SetColorKey(kDdckeySrcBlt, &key) == kDdOk);
    CHECK(destination->BltFast(1, 0, source, nullptr, kDdbltfastSrcColorKey) == kDdOk);
    CHECK(test::readPixels16(*destination) == std::vector<std::uint16_t>{5, 7, 5});
    CHECK(destination->BltFast(0, 0, source, nullptr, 0) == kDdOk);
    CHECK(test::readPixels16(*destination) == std::vector<std::uint16_t>{7, 9, 5});
    source->Release();
    destination->Release();
}

TEST_CASE("BltFast copies unstretched to a point and refuses what does not fit") {
    Rig rig;
    Surface *source = rig.Create(test::plainDesc(8, 8, PixelFormat::kRgb565));
    Surface *destination = rig.Create(test::plainDesc(16, 16, PixelFormat::kRgb565));
    giveTexture(rig, *source, 3);
    giveTexture(rig, *destination, 4);
    drawnByGpu(*destination);
    rig.gpu.log.Clear();
    const RectL part{2, 2, 6, 6};
    CHECK(destination->BltFast(10, 12, source, &part, 0x10) == kDdOk); // DDBLTFAST_WAIT
    CHECK(rig.gpu.log.Calls() == Calls{"Blit(3, 4, false)"});
    CHECK(rig.gpu.lastBlit.sourceRect.x == 2);
    CHECK(rig.gpu.lastBlit.destinationRect.x == 10);
    CHECK(rig.gpu.lastBlit.destinationRect.y == 12);
    CHECK(rig.gpu.lastBlit.destinationRect.width == 4);
    CHECK(destination->BltFast(13, 0, source, &part, 0) == kDdErrInvalidRect);
    CHECK(destination->BltFast(0, 0, source, &part, kDdbltfastDestColorKey) == kDdErrUnsupported);
    CHECK(destination->BltFast(0, 0, nullptr, &part, 0) == kDdErrInvalidParams);
    source->Release();
    destination->Release();
}

TEST_CASE("the frame is presented with the filter fix_smooth_scaled_frame names, sent once") {
    Rig rig;
    REQUIRE(rig.dd->SetDisplayMode(800, 600, 16, 0, 0) == kDdOk);
    Surface *front = rig.Create(test::flipChainDesc());
    rig.gpu.log.Clear();
    // On by default: linear, given before the first present only.
    CHECK(front->Flip(nullptr, kDdflipWait) == kDdOk);
    CHECK(rig.gpu.log.Calls() == Calls{"WriteTexture(1, 0, 0, 0, 800, 600, 1600)",
                                       "SetPresentFilter(1)", "Present()"});
    rig.gpu.log.Clear();
    CHECK(front->Flip(nullptr, kDdflipWait) == kDdOk);
    CHECK(rig.gpu.log.Count("SetPresentFilter") == 0);

    // Off: nearest, given once at the next present.
    fixes::Settings off;
    REQUIRE(off.applyCommandLine({"fix_smooth_scaled_frame=off"}).empty());
    fixes::install(off);
    rig.gpu.log.Clear();
    CHECK(front->Flip(nullptr, kDdflipWait) == kDdOk);
    CHECK(front->Flip(nullptr, kDdflipWait) == kDdOk);
    CHECK(rig.gpu.log.Count("SetPresentFilter") == 1);
    const Calls offCalls = rig.gpu.log.Calls();
    CHECK(std::find(offCalls.begin(), offCalls.end(), "SetPresentFilter(0)") != offCalls.end());
    fixes::install(fixes::Settings());

    // A Gpu that does not know the call still presents.
    rig.gpu.failOnly = "SetPresentFilter";
    rig.gpu.failWith = Error::kUnsupported;
    CHECK(front->Flip(nullptr, kDdflipWait) == kDdOk);
    CHECK(rig.gpu.log.Count("Present") == 3);
}

TEST_CASE("each present gives the window the whole-number scale fix_integer_scaled_frame names") {
    Rig rig;
    REQUIRE(rig.dd->SetDisplayMode(800, 600, 16, 0, 0) == kDdOk);
    Surface *front = rig.Create(test::flipChainDesc());
    rig.window.log.Clear();
    // Off by default.
    CHECK(front->Flip(nullptr, kDdflipWait) == kDdOk);
    CHECK(rig.window.log.Calls() == Calls{"SetWholeScale(false)"});

    fixes::Settings on;
    REQUIRE(on.applyCommandLine({"fix_integer_scaled_frame=on"}).empty());
    fixes::install(on);
    rig.window.log.Clear();
    CHECK(front->Flip(nullptr, kDdflipWait) == kDdOk);
    CHECK(rig.window.log.Calls() == Calls{"SetWholeScale(true)"});
    fixes::install(fixes::Settings());

    // A Window that does not know the call still presents.
    rig.window.setWholeScaleResult = Error::kUnsupported;
    rig.gpu.log.Clear();
    CHECK(front->Flip(nullptr, kDdflipWait) == kDdOk);
    CHECK(rig.gpu.log.Count("Present") == 1);
}

TEST_CASE("a resized window changes nothing the game's frame costs: the same calls per frame") {
    Rig rig;
    REQUIRE(rig.dd->SetDisplayMode(1024, 768, 16, 0, 0) == kDdOk);
    Surface *front = rig.Create(test::flipChainDesc());
    Surface *back = nullptr;
    REQUIRE(front->GetAttachedSurface(SurfaceCaps{kDdscapsBackBuffer, 0, 0, 0}, &back) == kDdOk);
    REQUIRE(front->Flip(nullptr, kDdflipWait) == kDdOk); // the first present gives the filter
    const auto frame = [&] {
        rig.gpu.log.Clear();
        SurfaceDesc locked;
        REQUIRE(back->Lock(nullptr, locked, kDdlockWait) == kDdOk);
        REQUIRE(back->Unlock() == kDdOk);
        REQUIRE(front->Flip(nullptr, kDdflipWait) == kDdOk);
        return rig.gpu.log.Calls();
    };
    const Calls oneFrame{"WriteTexture(1, 0, 0, 0, 1024, 768, 2048)", "Present()"};
    for (const auto &size : {std::pair<std::uint32_t, std::uint32_t>{800, 500},
                             {1024, 768},
                             {1920, 1080}}) {
        platform::WindowState state;
        state.width = 1024;
        state.height = 768;
        state.drawableWidth = size.first;
        state.drawableHeight = size.second;
        rig.window.stateResult = state;
        CAPTURE(size.first);
        CHECK(frame() == oneFrame);
        CHECK(back->Width() == 1024);
        CHECK(back->Height() == 768);
    }
}

TEST_CASE("Flip copies the back buffer into the display and presents it") {
    Rig rig;
    REQUIRE(rig.dd->SetDisplayMode(800, 600, 16, 0, 0) == kDdOk);
    Surface *front = rig.Create(test::flipChainDesc());
    Surface *back = nullptr;
    REQUIRE(front->GetAttachedSurface(SurfaceCaps{kDdscapsBackBuffer, 0, 0, 0}, &back) == kDdOk);
    rig.gpu.log.Clear();
    // Both host copies are current (the 2D screens): the copy is made there and the display
    // gets one upload.
    CHECK(front->Flip(nullptr, kDdflipWait) == kDdOk);
    CHECK(rig.gpu.log.Calls() == Calls{"WriteTexture(1, 0, 0, 0, 800, 600, 1600)",
                                       "SetPresentFilter(1)", "Present()"});

    // The game's frame: lock the back buffer, draw, unlock, flip.
    rig.gpu.log.Clear();
    SurfaceDesc locked;
    REQUIRE(back->Lock(nullptr, locked, kDdlockWait) == kDdOk);
    REQUIRE(back->Unlock() == kDdOk);
    CHECK(front->Flip(nullptr, kDdflipWait) == kDdOk);
    CHECK(rig.gpu.log.Calls() == Calls{"WriteTexture(1, 0, 0, 0, 800, 600, 1600)", "Present()"});

    // A frame Direct3D drew into the back buffer is copied on the Gpu.
    rig.gpu.createTextureResult = TextureId{5};
    REQUIRE(back->GpuTexture().Ok());
    CHECK(rig.gpu.lastTextureDesc.renderTarget);
    drawnByGpu(*back);
    rig.gpu.log.Clear();
    CHECK(front->Flip(nullptr, kDdflipWait) == kDdOk);
    CHECK(rig.gpu.log.Calls() == Calls{"Blit(5, 1, false)", "Present()"});
    CHECK(back->Flip(nullptr, 0) == kDdErrNotFlippable);
    back->Release();
    front->Release();
}

TEST_CASE("Flip at 8 bits converts the primary through its palette") {
    Rig rig;
    REQUIRE(rig.dd->SetDisplayMode(640, 480, 8, 0, 0) == kDdOk);
    Surface *front = rig.Create(test::flipChainDesc());
    std::array<PaletteColor, 256> colors{};
    Palette *palette = nullptr;
    REQUIRE(rig.dd->CreatePalette(kDdpcaps8Bit, colors.data(), &palette) == kDdOk);
    rig.gpu.createTextureResult = TextureId{5};
    CHECK(front->SetPalette(palette) == kDdOk);
    rig.gpu.log.Clear();
    CHECK(front->Flip(nullptr, kDdflipWait) == kDdOk);
    CHECK(rig.gpu.log.Calls() ==
          Calls{"WriteTexture(5, 0, 0, 0, 640, 480, 640)", "Blit(5, 1, false)", "Present()"});
    palette->Release();
    front->Release();
}

TEST_CASE("a primary without a back buffer is presented after each change") {
    Rig rig;
    REQUIRE(rig.dd->SetDisplayMode(640, 480, 16, 0, 0) == kDdOk);
    Surface *primary = rig.Create(test::primaryDesc());
    rig.gpu.log.Clear();
    const RectL rect{0, 0, 10, 10};
    SurfaceDesc locked;
    REQUIRE(primary->Lock(&rect, locked, 0) == kDdOk);
    CHECK(primary->Unlock() == kDdOk);
    CHECK(rig.gpu.log.Calls() == Calls{"WriteTexture(1, 0, 0, 0, 640, 480, 1280)",
                                       "SetPresentFilter(1)", "Present()"});
    primary->Release();
}

TEST_CASE("a primary with a flip chain is presented after a direct write until the game flips") {
    Rig rig;
    REQUIRE(rig.dd->SetDisplayMode(800, 600, 16, 0, 0) == kDdOk);
    Surface *front = rig.Create(test::flipChainDesc());
    rig.gpu.createTextureResult = TextureId{5};
    SurfaceDesc locked;
    rig.gpu.log.Clear();
    REQUIRE(front->Lock(nullptr, locked, 0) == kDdOk);
    REQUIRE(front->Unlock() == kDdOk);
    CHECK(rig.gpu.log.Count("Present") == 1); // not flipping yet: the write is what is shown
    CHECK(front->Flip(nullptr, kDdflipWait) == kDdOk);
    rig.gpu.log.Clear();
    REQUIRE(front->Lock(nullptr, locked, 0) == kDdOk);
    REQUIRE(front->Unlock() == kDdOk);
    CHECK(rig.gpu.log.Count("Present") == 0); // flipping: Flip presents
    front->Release();
}

TEST_CASE("a failed read back keeps the host copy stale") {
    Rig rig;
    Surface *surface = rig.Create(test::plainDesc(4, 2, PixelFormat::kRgb565));
    giveTexture(rig, *surface, 2);
    surface->MarkGpuWritten();
    rig.gpu.log.Clear();
    rig.gpu.failWith = platform::Error::kOutOfResources;
    rig.gpu.failOnly = "ReadTexture";
    surface->SyncToHost();
    rig.gpu.failWith = platform::Error::kNone;
    surface->SyncToHost(); // still stale: reads again, and now succeeds
    surface->SyncToHost(); // current: nothing to read
    CHECK(rig.gpu.log.Count("ReadTexture") == 2);
    surface->Release();
}

TEST_CASE("AddAttachedSurface holds a z-buffer until it is detached") {
    Rig rig;
    REQUIRE(rig.dd->SetDisplayMode(800, 600, 16, 0, 0) == kDdOk);
    Surface *front = rig.Create(test::flipChainDesc());
    Surface *back = nullptr;
    REQUIRE(front->GetAttachedSurface(SurfaceCaps{kDdscapsBackBuffer, 0, 0, 0}, &back) == kDdOk);
    SurfaceDesc zDesc;
    zDesc.flags = kDdsdCaps | kDdsdWidth | kDdsdHeight | kDdsdPixelFormat;
    zDesc.width = 800;
    zDesc.height = 600;
    zDesc.caps.caps = kDdscapsZBuffer;
    zDesc.pixelFormat = PixelFormatDesc{kDdpfZBuffer, 0, 16, 0, 0xFFFF, 0, 0};
    Surface *z = rig.Create(zDesc);

    CHECK(back->AddAttachedSurface(z) == kDdOk);
    CHECK(z->RefCount() == 2);
    CHECK(back->ZBuffer() == z);
    CHECK(back->AddAttachedSurface(z) == kDdErrSurfaceAlreadyAttached);
    Surface *plain = rig.Create(test::plainDesc(800, 600, PixelFormat::kRgb565));
    CHECK(back->AddAttachedSurface(plain) == kDdErrCannotAttachSurface);

    Surface *found = nullptr;
    CHECK(back->GetAttachedSurface(SurfaceCaps{kDdscapsZBuffer, 0, 0, 0}, &found) == kDdOk);
    CHECK(found == z);
    CHECK(z->RefCount() == 3);
    found->Release();
    CHECK(front->GetAttachedSurface(SurfaceCaps{kDdscapsZBuffer, 0, 0, 0}, &found) ==
          kDdErrNotFound);
    CHECK(found == nullptr);

    CHECK(back->DeleteAttachedSurface(0, z) == kDdOk);
    CHECK(z->RefCount() == 1);
    CHECK(back->DeleteAttachedSurface(0, z) == kDdErrCannotDetachSurface);
    CHECK(back->AddAttachedSurface(z) == kDdOk);
    z->Release(); // the back buffer's reference keeps it
    CHECK_FALSE(test::wasDestroyed(z));
    back->Release();
    front->Release(); // takes its back buffer, which releases the z-buffer
    CHECK(test::wasDestroyed(back));
    CHECK(test::wasDestroyed(z));
    plain->Release();
}

TEST_CASE("a back buffer lives as long as its primary") {
    Rig rig;
    Surface *front = rig.Create(test::flipChainDesc());
    Surface *back = nullptr;
    REQUIRE(front->GetAttachedSurface(SurfaceCaps{kDdscapsBackBuffer, 0, 0, 0}, &back) == kDdOk);
    CHECK(back->Release() == 0);
    CHECK(test::destroyed().empty());
    CHECK(front->Release() == 0);
    CHECK(test::destroyed() == std::vector<const ComObject *>{back, front});
}

TEST_CASE("GetDC hands out the host pixels and what GDI drew is uploaded at the next use") {
    Rig rig;
    Surface *surface = rig.Create(test::plainDesc(4, 2, PixelFormat::kRgb565));
    giveTexture(rig, *surface, 2);
    rig.gpu.log.Clear();
    SurfaceDesc dc;
    CHECK(surface->GetDC(dc) == kDdOk);
    CHECK(dc.surface != nullptr);
    CHECK(dc.pitch == 8);
    SurfaceDesc again;
    CHECK(surface->GetDC(again) == kDdErrDcAlreadyCreated);
    CHECK(surface->Lock(nullptr, again, 0) == kDdErrSurfaceBusy);
    CHECK(surface->ReleaseDC() == kDdOk);
    CHECK(rig.gpu.log.Calls().empty());
    CHECK(surface->GpuTexture().Ok());
    CHECK(rig.gpu.log.Calls() == Calls{"WriteTexture(2, 0, 0, 0, 4, 2, 8)"});
    CHECK(surface->ReleaseDC() == kDdErrNoDc);
    surface->Release();
}

TEST_CASE("GetSurfaceDesc, GetPixelFormat and GetColorKey describe the surface") {
    Rig rig;
    Surface *surface = rig.Create(test::plainDesc(10, 3, PixelFormat::kArgb8888));
    SurfaceDesc desc;
    CHECK(surface->GetSurfaceDesc(desc) == kDdOk);
    CHECK(desc.flags == (kDdsdCaps | kDdsdHeight | kDdsdWidth | kDdsdPitch | kDdsdPixelFormat));
    CHECK(desc.width == 10);
    CHECK(desc.pitch == 40);
    CHECK(desc.surface == nullptr);

    PixelFormatDesc format;
    CHECK(surface->GetPixelFormat(format) == kDdOk);
    CHECK(format.flags == (kDdpfRgb | kDdpfAlphaPixels));
    CHECK(format.alphaMask == 0xFF000000u);

    ColorKey key;
    CHECK(surface->GetColorKey(kDdckeySrcBlt, key) == kDdErrNoColorKey);
    const ColorKey range{0x10, 0x20};
    CHECK(surface->SetColorKey(kDdckeySrcBlt | kDdckeyColorSpace, &range) == kDdOk);
    CHECK(surface->GetColorKey(kDdckeySrcBlt, key) == kDdOk);
    CHECK(key.low == 0x10);
    CHECK(key.high == 0x20);
    CHECK(surface->GetSurfaceDesc(desc) == kDdOk);
    CHECK((desc.flags & kDdsdCkSrcBlt) != 0);
    CHECK(desc.srcBlt.high == 0x20);
    CHECK(surface->GetColorKey(kDdckeyDestBlt, key) == kDdErrNoColorKey);
    surface->Release();
}

TEST_CASE("SetColorKey sets the texture's key to the key's low value") {
    Rig rig;
    Surface *surface = rig.Create(test::plainDesc(4, 4, PixelFormat::kRgb565));
    giveTexture(rig, *surface, 2);
    rig.gpu.log.Clear();
    const ColorKey key{0xF81F, 0xF81F};
    CHECK(surface->SetColorKey(kDdckeySrcBlt, &key) == kDdOk);
    CHECK(surface->SetColorKey(kDdckeySrcBlt, nullptr) == kDdOk);
    CHECK(surface->SetColorKey(kDdckeyDestBlt, &key) == kDdOk);
    CHECK(rig.gpu.log.Calls() == Calls{"SetColorKey(2, 63519)", "SetColorKey(2, \"none\")"});
    CHECK(surface->SetColorKey(0x10, &key) == kDdErrUnsupported); // DDCKEY_SRCOVERLAY
    CHECK(surface->SetColorKey(0, &key) == kDdErrInvalidParams);
    surface->Release();
}

TEST_CASE("SetPalette attaches a palette to 8-bit surfaces only, with a reference") {
    Rig rig;
    std::array<PaletteColor, 256> colors{};
    Palette *palette = nullptr;
    REQUIRE(rig.dd->CreatePalette(kDdpcaps8Bit, colors.data(), &palette) == kDdOk);
    Surface *direct = rig.Create(test::plainDesc(4, 4, PixelFormat::kRgb565));
    CHECK(direct->SetPalette(palette) == kDdErrNotPalettized);

    Surface *indexed = rig.Create(test::plainDesc(4, 4, PixelFormat::kIndexed8));
    giveTexture(rig, *indexed, 7);
    rig.gpu.log.Clear();
    Palette *got = nullptr;
    CHECK(indexed->GetPalette(&got) == kDdErrNoPaletteAttached);
    CHECK(indexed->SetPalette(palette) == kDdOk);
    CHECK(palette->RefCount() == 2);
    CHECK(rig.gpu.log.Calls() ==
          Calls{"SetColorKey(7, \"none\")", "CreatePalette()", "AttachPalette(7, 1)"});
    CHECK(indexed->GetPalette(&got) == kDdOk);
    CHECK(got == palette);
    CHECK(palette->RefCount() == 3);
    got->Release();
    CHECK(indexed->SetPalette(nullptr) == kDdOk);
    CHECK(palette->RefCount() == 1);
    palette->Release();
    direct->Release();
    indexed->Release();
}

TEST_CASE("SetClipper holds the clipper and GetClipper hands it out") {
    Rig rig;
    Surface *surface = rig.Create(test::plainDesc(4, 4, PixelFormat::kRgb565));
    Clipper *clipper = nullptr;
    REQUIRE(rig.dd->CreateClipper(0, &clipper) == kDdOk);
    Clipper *got = nullptr;
    CHECK(surface->GetClipper(&got) == kDdErrNoClipperAttached);
    CHECK(surface->SetClipper(clipper) == kDdOk);
    CHECK(clipper->RefCount() == 2);
    CHECK(surface->GetClipper(&got) == kDdOk);
    CHECK(got == clipper);
    got->Release();
    clipper->Release();
    surface->Release(); // releases the clipper too
    CHECK(test::wasDestroyed(clipper));
}

TEST_CASE("surfaces are never lost and change their uniqueness value when written") {
    Rig rig;
    Surface *surface = rig.Create(test::plainDesc(4, 4, PixelFormat::kRgb565));
    CHECK(surface->IsLost() == kDdOk);
    CHECK(surface->Restore() == kDdOk);
    const std::uint32_t before = surface->UniquenessValue();
    test::writePixels16(*surface, std::vector<std::uint16_t>(16, 3));
    CHECK(surface->UniquenessValue() != before);
    surface->Release();
}

// ---- one upload per surface per frame ----

TEST_CASE("ten Lock/Unlock cycles and one draw upload the merged area once") {
    Rig rig;
    Surface *surface = rig.Create(test::plainDesc(64, 64, PixelFormat::kRgb565));
    giveTexture(rig, *surface, 2);
    rig.gpu.log.Clear();
    for (std::int32_t i = 0; i < 10; ++i) { // rectangles from (2,3)-(6,7) to (20,21)-(24,25)
        touch(*surface, RectL{2 + 2 * i, 3 + 2 * i, 6 + 2 * i, 7 + 2 * i});
    }
    CHECK(rig.gpu.log.Count("WriteTexture") == 0);
    CHECK(surface->GpuTexture().Ok()); // what Direct3D does when it binds the texture
    CHECK(rig.gpu.log.Calls() == Calls{"WriteTexture(2, 0, 2, 3, 22, 22, 128)"});
    CHECK(surface->GpuTexture().Ok()); // a second draw: nothing changed since
    CHECK(rig.gpu.log.Count("WriteTexture") == 1);
    surface->Release();
}

TEST_CASE("ten Lock/Unlock cycles of the back buffer and one Flip upload the frame once") {
    Rig rig;
    REQUIRE(rig.dd->SetDisplayMode(640, 480, 16, 0, 0) == kDdOk);
    Surface *front = rig.Create(test::flipChainDesc());
    Surface *back = nullptr;
    REQUIRE(front->GetAttachedSurface(SurfaceCaps{kDdscapsBackBuffer, 0, 0, 0}, &back) == kDdOk);
    REQUIRE(front->Flip(nullptr, kDdflipWait) == kDdOk); // the game is flipping
    for (int frame = 0; frame < 3; ++frame) {
        rig.gpu.log.Clear();
        for (std::int32_t i = 0; i < 10; ++i) {
            touch(*back, RectL{10 * i, 0, 10 * i + 8, 8});
        }
        CHECK(rig.gpu.log.Calls().empty());
        CHECK(front->Flip(nullptr, kDdflipWait) == kDdOk);
        CHECK(rig.gpu.log.Calls() ==
              Calls{"WriteTexture(1, 0, 0, 0, 640, 480, 1280)", "Present()"});
    }
    CHECK(rig.dd->Counted().uploads == 4); // the first Flip's and one a frame
    CHECK(rig.dd->Counted().presents == 4);
    back->Release();
    front->Release();
}

TEST_CASE("a host write inside a scene survives MarkGpuWritten and the next Lock") {
    Rig rig;
    Surface *back = rig.Create(test::plainDesc(16, 16, PixelFormat::kRgb565));
    giveTexture(rig, *back, 3); // BeginScene: the target's texture is bound and flushed
    rig.gpu.log.Clear();
    touch(*back, RectL{4, 4, 8, 8}); // the game locks the target mid-scene and writes
    CHECK(rig.gpu.log.Calls().empty());
    back->MarkGpuWritten(); // EndScene: the Gpu drew into the target
    // The write is uploaded before the Gpu copy becomes the source of truth.
    CHECK(rig.gpu.log.Calls() == Calls{"WriteTexture(3, 0, 4, 4, 4, 4, 32)"});
    SurfaceDesc locked;
    CHECK(back->Lock(nullptr, locked, 0) == kDdOk); // reads the Gpu copy, which has the write
    CHECK(rig.gpu.log.Count("ReadTexture") == 1);
    CHECK(back->Unlock() == kDdOk);
    back->Release();
}

TEST_CASE("Blt, BltFast and fills between current host copies upload the destination once") {
    Rig rig;
    Surface *sprite = rig.Create(test::plainDesc(8, 8, PixelFormat::kRgb565));
    Surface *screen = rig.Create(test::plainDesc(64, 64, PixelFormat::kRgb565));
    test::writePixels16(*sprite, std::vector<std::uint16_t>(64, 0x1234));
    giveTexture(rig, *screen, 4);
    rig.gpu.log.Clear();
    BltFx fx;
    fx.fillColor = 0x0001;
    const RectL band{0, 40, 64, 48};
    CHECK(screen->Blt(&band, nullptr, nullptr, kDdbltColorFill, &fx) == kDdOk);
    for (std::uint32_t i = 0; i < 5; ++i) {
        CHECK(screen->BltFast(8 * i, 8, sprite, nullptr, 0) == kDdOk);
        const RectL to{static_cast<std::int32_t>(8 * i), 20, static_cast<std::int32_t>(8 * i) + 4,
                       24};
        CHECK(screen->Blt(&to, sprite, nullptr, 0, nullptr) == kDdOk);
        touch(*screen, RectL{0, 0, 2, 2});
    }
    CHECK(rig.gpu.log.Calls().empty()); // no texture for the sprite, no upload, no blit
    CHECK(screen->GpuTexture().Ok());
    CHECK(rig.gpu.log.Calls() == Calls{"WriteTexture(4, 0, 0, 0, 64, 48, 128)"});
    const std::vector<std::uint16_t> pixels = test::readPixels16(*screen);
    CHECK(pixels[8 * 64 + 0] == 0x1234);      // BltFast
    CHECK(pixels[20 * 64 + 35] == 0x1234);    // the stretched Blt
    CHECK(pixels[44 * 64 + 63] == 0x0001);    // the fill
    CHECK(pixels[30 * 64 + 0] == 0);          // untouched
    sprite->Release();
    screen->Release();
}

TEST_CASE("a mipmap level uploads only when it changed, at the texture's next use") {
    Rig rig;
    SurfaceDesc desc = test::plainDesc(16, 16, PixelFormat::kRgb565);
    desc.flags |= kDdsdMipMapCount;
    desc.caps.caps = kDdscapsTexture | kDdscapsMipMap | kDdscapsComplex;
    desc.mipMapCount = 3;
    Surface *top = rig.Create(desc);
    Surface *level = nullptr;
    REQUIRE(top->GetAttachedSurface(SurfaceCaps{kDdscapsMipMap, 0, 0, 0}, &level) == kDdOk);
    giveTexture(rig, *top, 6); // every level's first upload
    CHECK(rig.gpu.log.Count("WriteTexture") == 3);
    rig.gpu.log.Clear();
    touch(*level, RectL{0, 0, 4, 4});
    touch(*level, RectL{4, 4, 8, 8});
    CHECK(rig.gpu.log.Calls().empty());
    CHECK(top->GpuTexture().Ok());
    CHECK(rig.gpu.log.Calls() == Calls{"WriteTexture(6, 1, 0, 0, 8, 8, 16)"});
    level->Release();
    top->Release();
}

TEST_CASE("the main menu's frame uploads the back buffer once and presents it") {
    // The game's menu frame, as a call trace of the running game shows it: Clear2 on the back
    // buffer (a colour fill), BeginScene and EndScene with nothing drawn, three Lock/Unlock of
    // the whole back buffer, Flip; some frames also lock the back buffer before the clear.
    Rig rig;
    REQUIRE(rig.dd->SetDisplayMode(800, 600, 16, 0, 0) == kDdOk);
    Surface *front = rig.Create(test::flipChainDesc());
    Surface *back = nullptr;
    REQUIRE(front->GetAttachedSurface(SurfaceCaps{kDdscapsBackBuffer, 0, 0, 0}, &back) == kDdOk);
    giveTexture(rig, *back, 5); // BeginScene's first use: a render target
    REQUIRE(front->Flip(nullptr, kDdflipWait) == kDdOk);
    BltFx clear;
    clear.fillColor = 0;
    for (std::uint16_t frame = 1; frame <= 4; ++frame) {
        rig.gpu.log.Clear();
        Calls expected;
        if (frame == 2) {
            touch(*back, RectL{0, 0, 800, 600}); // the clear covers it: nothing to upload first
        }
        if (frame == 3) {
            touch(*front, RectL{0, 0, 800, 600}); // the next Flip covers it: nothing to upload
            expected.push_back("ReadTexture(1, 0, 0, 0, 800, 600, 1600)");
        }
        REQUIRE(back->Blt(nullptr, nullptr, nullptr, kDdbltColorFill, &clear) == kDdOk);
        REQUIRE(back->GpuTexture().Ok()); // BeginScene
        for (int i = 0; i < 3; ++i) {
            test::writePixels16(*back, std::vector<std::uint16_t>(800 * 600, frame));
        }
        REQUIRE(front->Flip(nullptr, kDdflipWait) == kDdOk);
        for (const char *call : {"Fill(5, 0, 0, 800, 600, 0)",
                                 "WriteTexture(5, 0, 0, 0, 800, 600, 1600)", "Blit(5, 1, false)",
                                 "Present()"}) {
            expected.push_back(call);
        }
        CHECK(rig.gpu.log.Calls() == expected);
    }
    back->Release();
    front->Release();
}

} // namespace sfcop::hle::ddraw4
