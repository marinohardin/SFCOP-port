#include "hle/ddraw4/Clipper.h"
#include "hle/ddraw4/Ddraw4TestRig.h"
#include "hle/ddraw4/Palette.h"

#include "fixes/fixes.h"

namespace sfcop::hle::ddraw4 {

using platform::Error;
using platform::PixelFormat;
using test::Calls;
using test::Rig;

TEST_CASE("a DirectDraw object counts references and dies at its last Release") {
    platform::GpuMock gpu;
    platform::WindowMock window;
    HeapPixelMemory memory;
    test::destroyed().clear();
    ComObject::SetDestroyHook(&test::recordDestroyed);
    DirectDraw *dd = new DirectDraw(gpu, window, memory);
    CHECK(dd->RefCount() == 1);
    CHECK(dd->AddRef() == 2); // QueryInterface for IDirectDraw4 adds to the one count
    CHECK(dd->Release() == 1);
    CHECK(test::destroyed().empty());
    CHECK(dd->Release() == 0);
    CHECK(test::destroyed() == std::vector<const ComObject *>{dd});
    ComObject::SetDestroyHook(nullptr);
}

TEST_CASE("a DirectDraw object outlives the game's references while its surfaces live") {
    Rig rig;
    Surface *surface = rig.Create(test::plainDesc(8, 8, PixelFormat::kRgb565));
    CHECK(rig.dd->Release() == 0);
    CHECK(test::destroyed().empty());
    CHECK(surface->Release() == 0);
    CHECK(test::destroyed().size() == 2);
    CHECK(test::wasDestroyed(surface));
    CHECK(test::wasDestroyed(rig.dd));
}

TEST_CASE("SetCooperativeLevel takes fullscreen exclusive with a window, or normal") {
    Rig rig;
    CHECK(rig.dd->SetCooperativeLevel(0x00010001, kDdsclFullscreen | kDdsclExclusive) == kDdOk);
    CHECK(rig.dd->CooperativeWindow() == 0x00010001);
    CHECK(rig.dd->SetCooperativeLevel(0x00010001, kDdsclNormal) == kDdOk);
    CHECK(rig.dd->SetCooperativeLevel(0x00010001, kDdsclFullscreen) == kDdErrInvalidParams);
    CHECK(rig.dd->SetCooperativeLevel(0, kDdsclFullscreen | kDdsclExclusive) ==
          kDdErrInvalidParams);
    CHECK(rig.dd->SetCooperativeLevel(0x00010001, 0) == kDdErrInvalidParams);
}

TEST_CASE("SetDisplayMode sizes the window in its mode and makes the back buffer") {
    Rig rig;
    CHECK(rig.dd->SetDisplayMode(800, 600, 16, 0, 0) == kDdOk);
    CHECK(rig.window.log.Calls() == Calls{"GetState()", "SetMode(0, 800, 600)"});
    CHECK(rig.gpu.log.Calls() == Calls{"ConfigureBackBuffer(800, 600, 0)"});

    SurfaceDesc mode;
    CHECK(rig.dd->GetDisplayMode(mode) == kDdOk);
    CHECK(mode.flags ==
          (kDdsdHeight | kDdsdWidth | kDdsdPitch | kDdsdPixelFormat | kDdsdRefreshRate));
    CHECK(mode.width == 800);
    CHECK(mode.height == 600);
    CHECK(mode.pitch == 1600);
    CHECK(mode.mipMapCount == 60);
    CHECK(mode.pixelFormat.flags == kDdpfRgb);
    CHECK(mode.pixelFormat.bitCount == 16);
    CHECK(mode.pixelFormat.redMask == 0xF800);
    CHECK(mode.pixelFormat.greenMask == 0x07E0);
    CHECK(mode.pixelFormat.blueMask == 0x001F);
}

TEST_CASE("an 8-bit display mode shows through a 32-bit back buffer") {
    Rig rig;
    CHECK(rig.dd->SetDisplayMode(640, 480, 8, 0, 0) == kDdOk);
    CHECK(rig.gpu.log.Calls() == Calls{"ConfigureBackBuffer(640, 480, 2)"});
    CHECK(rig.dd->PrimaryFormat() == PixelFormat::kIndexed8);
}

TEST_CASE("SetDisplayMode refuses depths and sizes the display cannot take") {
    Rig rig;
    CHECK(rig.dd->SetDisplayMode(800, 600, 24, 0, 0) == kDdErrInvalidMode);
    CHECK(rig.window.log.Calls().empty());
    rig.window.setModeResult = Error::kInvalidArgument;
    CHECK(rig.dd->SetDisplayMode(20000, 600, 16, 0, 0) == kDdErrInvalidMode);
    CHECK(rig.gpu.log.Calls().empty());
    rig.window.setModeResult = {};
    rig.gpu.backBufferResult = Error::kInvalidArgument;
    CHECK(rig.dd->SetDisplayMode(800, 600, 16, 0, 0) == kDdErrInvalidMode);
    SurfaceDesc mode;
    CHECK(rig.dd->GetDisplayMode(mode) == kDdOk);
    CHECK(mode.width == 640); // unchanged by the failed calls
}

TEST_CASE("before SetDisplayMode the display mode is 640x480 at 16 bits") {
    Rig rig;
    SurfaceDesc mode;
    CHECK(rig.dd->GetDisplayMode(mode) == kDdOk);
    CHECK(mode.width == 640);
    CHECK(mode.height == 480);
    CHECK(mode.pitch == 1280);
    CHECK(mode.pixelFormat.bitCount == 16);
}

TEST_CASE("EnumDisplayModes offers the classic and listed modes up to the desktop's size") {
    Rig rig;
    rig.window.desktopModeResult = platform::DisplayMode{1024, 768, 60};
    rig.window.displayModesResult =
        std::vector<platform::DisplayMode>{{1366, 768, 60}, {1024, 768, 60}, {720, 400, 70}};
    std::vector<SurfaceDesc> modes;
    CHECK(rig.dd->EnumDisplayModes(0, nullptr, modes) == kDdOk);
    REQUIRE(modes.size() == 15);
    CHECK(modes[0].width == 640);
    CHECK(modes[0].height == 480);
    CHECK(modes[0].pixelFormat.bitCount == 8);
    CHECK(modes[0].pixelFormat.flags == (kDdpfRgb | kDdpfPaletteIndexed8));
    CHECK(modes[1].width == 720);
    CHECK(modes[1].height == 400);
    CHECK(modes[3].width == 960);
    CHECK(modes[3].height == 720);
    CHECK(modes[4].width == 1024);
    CHECK(modes[5].pixelFormat.bitCount == 16);
    CHECK(modes[5].pitch == 1280);
    CHECK(modes[14].width == 1024);
    CHECK(modes[14].pixelFormat.bitCount == 32);
    CHECK(modes[14].pitch == 4096);

    SurfaceDesc filter;
    filter.flags = kDdsdWidth | kDdsdPixelFormat;
    filter.width = 800;
    filter.pixelFormat.bitCount = 16;
    CHECK(rig.dd->EnumDisplayModes(0, &filter, modes) == kDdOk);
    REQUIRE(modes.size() == 1);
    CHECK(modes[0].height == 600);
    CHECK(modes[0].mipMapCount == 60);
}

TEST_CASE("EnumDisplayModes lists every size the game's lowres setting can ask for, smallest first") {
    Rig rig;
    rig.window.desktopModeResult = platform::DisplayMode{1600, 1200, 60};
    rig.window.displayModesResult = std::vector<platform::DisplayMode>{};
    SurfaceDesc filter;
    filter.flags = kDdsdPixelFormat;
    filter.pixelFormat.bitCount = 16;
    std::vector<SurfaceDesc> modes;
    CHECK(rig.dd->EnumDisplayModes(0, &filter, modes) == kDdOk);
    REQUIRE(modes.size() == 8);
    CHECK(modes[0].width == 640);
    CHECK(modes[0].height == 480);
    CHECK(modes[1].width == 800);
    CHECK(modes[1].height == 600);
    CHECK(modes[2].width == 960);
    CHECK(modes[2].height == 720);
    CHECK(modes[2].pitch == 1920);
    CHECK(modes[3].width == 1024);
    CHECK(modes[3].height == 768);
    CHECK(modes[4].width == 1152);
    CHECK(modes[4].height == 864);
    CHECK(modes[5].width == 1280);
    CHECK(modes[5].height == 960);
    CHECK(modes[6].width == 1280);
    CHECK(modes[6].height == 1024);
    CHECK(modes[7].width == 1600);
    CHECK(modes[7].height == 1200);
}

TEST_CASE("GetCaps reports blits, colour keys, 8-bit palettes and 16-bit z-buffers") {
    Rig rig;
    DdCaps driver;
    DdCaps emulation;
    CHECK(rig.dd->GetCaps(&driver, &emulation) == kDdOk);
    CHECK(driver.caps == 0x04408241u);
    CHECK(driver.colorKeyCaps == 0x200u);
    CHECK(driver.paletteCaps == 0x54u);
    CHECK(driver.zBufferBitDepths == 0x400u);
    CHECK(driver.vidMemTotal == 268435456u);
    CHECK(driver.vidMemFree == 201326592u);
    CHECK(driver.rops[0] == 0x1u);                                 // BLACKNESS, index 0x00
    CHECK(driver.rops[6] == 0x1000u);                              // SRCCOPY, index 0xCC
    CHECK(driver.rops[7] == 0x80000000u);                          // WHITENESS, index 0xFF
    CHECK((driver.surfaceCaps.caps & 0x00022200u) == 0x00022200u); // primary, 3D device, z
    CHECK(emulation.caps == driver.caps);
    CHECK(rig.dd->GetCaps(nullptr, nullptr) == kDdErrInvalidParams);
}

TEST_CASE("CreateSurface makes the game's flipping primary at the display mode") {
    Rig rig;
    REQUIRE(rig.dd->SetDisplayMode(800, 600, 16, 0, 0) == kDdOk);
    Surface *front = rig.Create(test::flipChainDesc());
    CHECK(front->Role() == SurfaceRole::kPrimary);
    CHECK(front->Width() == 800);
    CHECK(front->Height() == 600);
    SurfaceCaps caps;
    CHECK(front->GetCaps(caps) == kDdOk);
    CHECK(caps.caps ==
          (kDdscapsPrimarySurface | kDdscapsFlip | kDdscapsComplex | kDdscaps3dDevice |
           kDdscapsVisible | kDdscapsFrontBuffer | kDdscapsVideoMemory | kDdscapsLocalVidMem));
    CHECK(rig.dd->Primary() == front);

    Surface *back = nullptr;
    CHECK(front->GetAttachedSurface(SurfaceCaps{kDdscapsBackBuffer, 0, 0, 0}, &back) == kDdOk);
    REQUIRE(back != nullptr);
    CHECK(back->Role() == SurfaceRole::kBackBuffer);
    CHECK(back->RefCount() == 1);
    CHECK(back->GetCaps(caps) == kDdOk);
    CHECK(caps.caps == (kDdscapsBackBuffer | kDdscapsFlip | kDdscapsComplex | kDdscaps3dDevice |
                        kDdscapsVideoMemory | kDdscapsLocalVidMem));
    CHECK(rig.gpu.log.Count("CreateTexture") == 0); // textures wait for first use
    back->Release();
    front->Release();
}

TEST_CASE("CreateSurface refuses a flipping primary without a back buffer count") {
    Rig rig;
    SurfaceDesc desc = test::flipChainDesc();
    desc.flags = kDdsdCaps;
    Surface *surface = reinterpret_cast<Surface *>(0x1);
    CHECK(rig.dd->CreateSurface(desc, &surface) == kDdErrInvalidCaps);
    CHECK(surface == nullptr);
    desc = test::flipChainDesc();
    desc.backBufferCount = 4;
    CHECK(rig.dd->CreateSurface(desc, &surface) == kDdErrInvalidParams);
}

TEST_CASE("CreateSurface checks the size and pixel format of other surfaces") {
    Rig rig;
    Surface *surface = nullptr;
    SurfaceDesc desc = test::plainDesc(16, 16, PixelFormat::kRgb565);
    desc.flags &= ~kDdsdWidth;
    CHECK(rig.dd->CreateSurface(desc, &surface) == kDdErrInvalidParams);

    desc = test::plainDesc(16, 16, PixelFormat::kRgb565);
    desc.pixelFormat = PixelFormatDesc{kDdpfFourCc, 0x31545844, 0, 0, 0, 0, 0}; // "DXT1"
    CHECK(rig.dd->CreateSurface(desc, &surface) == kDdErrInvalidPixelFormat);

    desc = test::plainDesc(16, 16, PixelFormat::kRgb565);
    desc.flags |= kDdsdLpSurface;
    CHECK(rig.dd->CreateSurface(desc, &surface) == kDdErrUnsupported);

    desc = SurfaceDesc{};
    desc.flags = kDdsdCaps | kDdsdWidth | kDdsdHeight;
    desc.width = 32;
    desc.height = 16;
    desc.caps.caps = kDdscapsSystemMemory;
    surface = rig.Create(desc); // no pixel format: the display's
    CHECK(surface->Format() == PixelFormat::kRgb565);
    SurfaceDesc made;
    CHECK(surface->GetSurfaceDesc(made) == kDdOk);
    CHECK(made.pitch == 64);
    CHECK(made.caps.caps == (kDdscapsSystemMemory | kDdscapsOffscreenPlain));
    surface->Release();
}

TEST_CASE("CreateSurface makes 16-bit z-buffers only") {
    Rig rig;
    SurfaceDesc desc;
    desc.flags = kDdsdCaps | kDdsdWidth | kDdsdHeight | kDdsdPixelFormat;
    desc.width = 800;
    desc.height = 600;
    desc.caps.caps = kDdscapsZBuffer | kDdscapsVideoMemory;
    desc.pixelFormat = PixelFormatDesc{kDdpfZBuffer, 0, 16, 0, 0xFFFF, 0, 0};
    Surface *z = rig.Create(desc);
    CHECK(z->Role() == SurfaceRole::kZBuffer);
    CHECK(z->Format() == PixelFormat::kDepth16);
    z->Release();

    desc.pixelFormat.bitCount = 24;
    Surface *refused = nullptr;
    CHECK(rig.dd->CreateSurface(desc, &refused) == kDdErrInvalidPixelFormat);

    desc.flags = kDdsdCaps | kDdsdWidth | kDdsdHeight | kDdsdZBufferBitDepth;
    desc.mipMapCount = 16; // dwZBufferBitDepth
    z = rig.Create(desc);
    CHECK(z->Format() == PixelFormat::kDepth16);
    z->Release();
}

TEST_CASE("CreateSurface makes a mipmapped texture's levels as attached surfaces") {
    Rig rig;
    SurfaceDesc desc = test::plainDesc(64, 32, PixelFormat::kArgb1555);
    desc.flags |= kDdsdMipMapCount;
    desc.caps.caps = kDdscapsTexture | kDdscapsMipMap | kDdscapsComplex;
    desc.mipMapCount = 3;
    Surface *top = rig.Create(desc);
    SurfaceDesc made;
    CHECK(top->GetSurfaceDesc(made) == kDdOk);
    CHECK((made.flags & kDdsdMipMapCount) != 0);
    CHECK(made.mipMapCount == 3);

    Surface *level = nullptr;
    CHECK(top->GetAttachedSurface(SurfaceCaps{kDdscapsMipMap, 0, 0, 0}, &level) == kDdOk);
    REQUIRE(level != nullptr);
    CHECK(level->Width() == 32);
    CHECK(level->Height() == 16);
    CHECK(level->Level() == 1);
    SurfaceCaps caps;
    CHECK(level->GetCaps(caps) == kDdOk);
    CHECK(caps.caps2 == 0x00010000u); // DDSCAPS2_MIPMAPSUBLEVEL
    level->Release();

    desc.mipMapCount = 7; // 64x32 has 6 levels
    Surface *refused = nullptr;
    CHECK(rig.dd->CreateSurface(desc, &refused) == kDdErrInvalidParams);
    top->Release();
}

TEST_CASE("CreateSurface hands out primaries made before a mode at 640x480") {
    Rig rig;
    Surface *probe = rig.Create(test::primaryDesc());
    CHECK(probe->Width() == 640);
    CHECK(probe->Height() == 480);
    CHECK(probe->Format() == PixelFormat::kRgb565);
    probe->Release();
    CHECK(rig.dd->Primary() == nullptr);
}

TEST_CASE("CreatePalette makes 8-bit palettes only") {
    Rig rig;
    std::array<PaletteColor, 256> colors{};
    Palette *palette = nullptr;
    CHECK(rig.dd->CreatePalette(kDdpcaps8Bit | kDdpcapsAllow256, colors.data(), &palette) == kDdOk);
    REQUIRE(palette != nullptr);
    palette->Release();
    CHECK(rig.dd->CreatePalette(kDdpcaps4Bit, colors.data(), &palette) == kDdErrUnsupported);
    CHECK(palette == nullptr);
    CHECK(rig.dd->CreatePalette(kDdpcaps8Bit, nullptr, &palette) == kDdErrInvalidParams);
}

TEST_CASE("CreateClipper makes a clipper and refuses flags") {
    Rig rig;
    Clipper *clipper = nullptr;
    CHECK(rig.dd->CreateClipper(0, &clipper) == kDdOk);
    REQUIRE(clipper != nullptr);
    CHECK(clipper->RefCount() == 1);
    clipper->Release();
    CHECK(rig.dd->CreateClipper(1, &clipper) == kDdErrInvalidParams);
}

TEST_CASE("GetGDISurface hands out the primary with a reference") {
    Rig rig;
    Surface *surface = nullptr;
    CHECK(rig.dd->GetGdiSurface(&surface) == kDdErrNotFound);
    Surface *primary = rig.Create(test::primaryDesc());
    CHECK(rig.dd->GetGdiSurface(&surface) == kDdOk);
    CHECK(surface == primary);
    CHECK(primary->RefCount() == 2);
    surface->Release();
    primary->Release();
}

TEST_CASE("the timing and memory queries give fixed answers") {
    Rig rig;
    std::uint32_t total = 0;
    std::uint32_t free = 0;
    CHECK(rig.dd->GetAvailableVidMem(SurfaceCaps{kDdscapsTexture, 0, 0, 0}, &total, &free) ==
          kDdOk);
    CHECK(total == 268435456u);
    CHECK(free == 201326592u);
    CHECK(rig.dd->GetAvailableVidMem(SurfaceCaps{}, nullptr, nullptr) == kDdErrInvalidParams);
    CHECK(rig.dd->WaitForVerticalBlank(1) == kDdOk);
    CHECK(rig.dd->WaitForVerticalBlank(2) == kDdErrUnsupported);
    std::uint32_t hertz = 0;
    CHECK(rig.dd->GetMonitorFrequency(hertz) == kDdOk);
    CHECK(hertz == 60);
    bool inBlank = true;
    CHECK(rig.dd->GetVerticalBlankStatus(inBlank) == kDdOk);
    CHECK_FALSE(inBlank);
    std::uint32_t line = 7;
    CHECK(rig.dd->GetScanLine(line) == kDdOk);
    CHECK(line == 0);
    DeviceIdentifier id;
    CHECK(rig.dd->GetDeviceIdentifier(0, id) == kDdOk);
    CHECK(id.description == "Display adapter");
    CHECK(rig.dd->Initialize() == kDdErrAlreadyInitialized);
    CHECK(rig.dd->TestCooperativeLevel() == kDdOk);
}

TEST_CASE("pixel formats map to the Gpu's and back") {
    CHECK(formatOf(PixelFormatDesc{kDdpfRgb, 0, 16, 0xF800, 0x07E0, 0x001F, 0}) ==
          PixelFormat::kRgb565);
    CHECK(formatOf(PixelFormatDesc{kDdpfRgb, 0, 16, 0x7C00, 0x03E0, 0x001F, 0}) ==
          PixelFormat::kArgb1555);
    CHECK(formatOf(PixelFormatDesc{kDdpfRgb | kDdpfAlphaPixels, 0, 16, 0x7C00, 0x03E0, 0x001F,
                                   0x8000}) == PixelFormat::kArgb1555);
    CHECK(formatOf(PixelFormatDesc{kDdpfRgb | kDdpfAlphaPixels, 0, 16, 0x0F00, 0x00F0, 0x000F,
                                   0xF000}) == std::nullopt); // 4444: the Gpu has none
    CHECK(formatOf(PixelFormatDesc{kDdpfRgb | kDdpfAlphaPixels, 0, 32, 0xFF0000, 0xFF00, 0xFF,
                                   0xFF000000}) == PixelFormat::kArgb8888);
    CHECK(formatOf(PixelFormatDesc{kDdpfRgb | kDdpfPaletteIndexed8, 0, 8, 0, 0, 0, 0}) ==
          PixelFormat::kIndexed8);
    CHECK(describe(PixelFormat::kXrgb8888).redMask == 0x00FF0000u);
    CHECK(describe(PixelFormat::kArgb1555).alphaMask == 0x8000u);
    CHECK(packColor(PixelFormat::kRgb565, 0xFF102030) == 0x1106u);
    CHECK(packColor(PixelFormat::kArgb1555, 0x80FF0000) == 0xFC00u);
    CHECK(fromError(Error::kOutOfResources) == kDdErrOutOfVideoMemory);
}

TEST_CASE("each Gpu's display has its own owner") {
    Rig first;
    Rig second; // another Gpu and Window, and a DirectDraw object of its own
    REQUIRE(first.dd->SetDisplayMode(800, 600, 16, 0, 0) == kDdOk);
    CHECK(first.dd->OwnsDisplay());
    CHECK_FALSE(second.dd->OwnsDisplay());
    // The other Gpu's owner is no reason to refuse this object its display.
    CHECK(second.dd->DisplayTexture().Ok());
    CHECK(second.dd->OwnsDisplay());
    CHECK(first.dd->OwnsDisplay());
    CHECK(first.gpu.log.Count("ConfigureBackBuffer") == 1);
    CHECK(second.gpu.log.Count("ConfigureBackBuffer") == 1);
}

TEST_CASE("a second DirectDraw object never takes the display lazily") {
    // The game makes a DirectDraw object with a primary and never sets a mode on it, then makes
    // the one it plays with. Asking the first for the display makes no back buffer.
    Rig rig;
    Surface *early = rig.Create(test::primaryDesc());
    DirectDraw *game = new DirectDraw(rig.gpu, rig.window, rig.memory);
    REQUIRE(game->SetDisplayMode(800, 600, 16, 0, 0) == kDdOk);
    CHECK(rig.gpu.log.Count("ConfigureBackBuffer") == 1);
    CHECK(game->OwnsDisplay());
    CHECK_FALSE(rig.dd->OwnsDisplay());

    const platform::Result<platform::TextureId> taken = rig.dd->DisplayTexture();
    CHECK_FALSE(taken.Ok());
    CHECK(taken.Code() == Error::kWrongState);
    test::writePixels16(*early, std::vector<std::uint16_t>(640 * 480, 7)); // presents nothing
    CHECK(rig.gpu.log.Count("ConfigureBackBuffer") == 1);
    CHECK(rig.gpu.log.Count("Present") == 0);
    CHECK(game->OwnsDisplay());

    const platform::Result<platform::TextureId> shown = game->DisplayTexture();
    REQUIRE(shown.Ok());
    CHECK(shown.Value() == platform::TextureId{1});
    CHECK(rig.gpu.log.Count("ConfigureBackBuffer") == 1);

    game->Release(); // the display has no owner now: the early object may make its own
    CHECK(rig.dd->DisplayTexture().Ok());
    CHECK(rig.dd->OwnsDisplay());
    CHECK(rig.gpu.log.Count("ConfigureBackBuffer") == 2);
    early->Release();
}

namespace {

// A window of `width` by `height` drawable pixels on a display of two pixels per point, and a
// Gpu whose textures may be `maxTexture` pixels on a side.
void giveWindow(Rig &rig, std::uint32_t width, std::uint32_t height, std::uint32_t maxTexture) {
    rig.window.stateResult = platform::WindowState{platform::WindowMode::kWindowed, width / 2,
                                                   height / 2, width, height, true, false};
    platform::GpuCaps caps;
    caps.maxTextureSize = maxTexture;
    rig.gpu.capsResult = caps;
}

} // namespace

TEST_CASE("the render scale is the frame placement's scale rounded up, from the same chooser") {
    Rig rig;
    REQUIRE(rig.dd->SetDisplayMode(800, 600, 16, 0, 0) == kDdOk);
    // 800x600 in 2560x1440 pixels: placed 1920x1440, a scale of 2.4, so 3.
    giveWindow(rig, 2560, 1440, 8192);
    CHECK(rig.dd->WantedRenderScale() == 3);
    // A window of exactly twice the frame: 2.
    giveWindow(rig, 1600, 1200, 8192);
    CHECK(rig.dd->WantedRenderScale() == 2);
    // 1366x768 points on a two-pixel display: 2732x1536, placed 2048x1536 (2.56), so 3.
    giveWindow(rig, 2732, 1536, 8192);
    CHECK(rig.dd->WantedRenderScale() == 3);
    // A window of the frame's own size, or a smaller one: 1.
    giveWindow(rig, 800, 600, 8192);
    CHECK(rig.dd->WantedRenderScale() == 1);
    giveWindow(rig, 640, 480, 8192);
    CHECK(rig.dd->WantedRenderScale() == 1);

    // With the whole-number scale on, the present places the frame at a whole scale, and the
    // render scale follows the same placement: 2560x1440 shows the frame at 2 (1600x1200), and
    // so does 2732x1536, where the fitted placement asked 3.
    fixes::Settings whole;
    REQUIRE(whole.applyCommandLine({"fix_integer_scaled_frame=on"}).empty());
    fixes::install(whole);
    giveWindow(rig, 2560, 1440, 8192);
    CHECK(rig.dd->WantedRenderScale() == 2);
    giveWindow(rig, 2732, 1536, 8192);
    CHECK(rig.dd->WantedRenderScale() == 2);
    giveWindow(rig, 2400, 1800, 8192);
    CHECK(rig.dd->WantedRenderScale() == 3);
    fixes::install(fixes::Settings());
}

TEST_CASE("the render scale stays within the Gpu's largest texture and the option") {
    Rig rig;
    REQUIRE(rig.dd->SetDisplayMode(800, 600, 16, 0, 0) == kDdOk);
    // 3 would be 2400 pixels wide: a Gpu of 2048 gives 2.
    giveWindow(rig, 2560, 1440, 2048);
    CHECK(rig.dd->WantedRenderScale() == 2);

    Rig small; // the largest texture is read once per object
    REQUIRE(small.dd->SetDisplayMode(800, 600, 16, 0, 0) == kDdOk);
    giveWindow(small, 2560, 1440, 1024);
    CHECK(small.dd->WantedRenderScale() == 1);

    Rig option;
    REQUIRE(option.dd->SetDisplayMode(800, 600, 16, 0, 0) == kDdOk);
    giveWindow(option, 2560, 1440, 8192);
    DirectDraw::SetRenderScaleOption(2);
    CHECK(option.dd->WantedRenderScale() == 2);
    DirectDraw::SetRenderScaleOption(1);
    CHECK(option.dd->WantedRenderScale() == 1);
    DirectDraw::SetRenderScaleOption(5);
    CHECK(option.dd->WantedRenderScale() == 3); // a cap, not a demand
    DirectDraw::SetRenderScaleOption(0);
    CHECK(option.dd->WantedRenderScale() == 3);
}

TEST_CASE("the render scale is 1 with fix_render_scale off, at 8 bits, or with no window state") {
    Rig rig;
    REQUIRE(rig.dd->SetDisplayMode(800, 600, 16, 0, 0) == kDdOk);
    giveWindow(rig, 2560, 1440, 8192);
    fixes::Settings off;
    REQUIRE(off.applyCommandLine({"fix_render_scale=off"}).empty());
    fixes::install(off);
    CHECK(rig.dd->WantedRenderScale() == 1);
    fixes::install(fixes::Settings());
    CHECK(rig.dd->WantedRenderScale() == 3);

    rig.window.stateResult = Error::kWrongState;
    CHECK(rig.dd->WantedRenderScale() == 1);

    Rig indexed;
    REQUIRE(indexed.dd->SetDisplayMode(800, 600, 8, 0, 0) == kDdOk);
    giveWindow(indexed, 2560, 1440, 8192);
    CHECK(indexed.dd->WantedRenderScale() == 1);
}

TEST_CASE("a scene's start makes the display again at the scale the window asks for") {
    Rig rig;
    REQUIRE(rig.dd->SetDisplayMode(800, 600, 16, 0, 0) == kDdOk);
    giveWindow(rig, 2560, 1440, 8192);
    CHECK(rig.dd->DisplayScale() == 1);
    rig.gpu.log.Clear();
    rig.gpu.backBufferResult = platform::TextureId{9};
    CHECK(rig.dd->BeginSceneScale() == 3);
    CHECK(rig.gpu.log.Calls() == Calls{"GetCaps()", "ConfigureScaledBackBuffer(800, 600, 0, 3)"});
    CHECK(rig.dd->DisplayScale() == 3);
    REQUIRE(rig.dd->DisplayTexture().Ok());
    CHECK(rig.dd->DisplayTexture().Value() == platform::TextureId{9});

    // The same window again: nothing is made.
    rig.gpu.log.Clear();
    CHECK(rig.dd->BeginSceneScale() == 3);
    CHECK(rig.gpu.log.Calls().empty());

    // A resize: the next scene's start makes it again; back to the frame's size, at 1.
    giveWindow(rig, 800, 600, 8192);
    rig.gpu.backBufferResult = platform::TextureId{10};
    CHECK(rig.dd->BeginSceneScale() == 1);
    CHECK(rig.gpu.log.Calls() == Calls{"ConfigureBackBuffer(800, 600, 0)"});
    CHECK(rig.dd->DisplayScale() == 1);

    // A mode change puts the display back to 1 until the next scene.
    giveWindow(rig, 2560, 1440, 8192);
    REQUIRE(rig.dd->BeginSceneScale() == 3);
    rig.gpu.log.Clear();
    REQUIRE(rig.dd->SetDisplayMode(800, 600, 16, 0, 0) == kDdOk);
    CHECK(rig.gpu.log.Calls() == Calls{"ConfigureBackBuffer(800, 600, 0)"});
    CHECK(rig.dd->DisplayScale() == 1);
}

TEST_CASE("a Gpu that refuses a scaled back buffer keeps the scene at 1, asked once per mode") {
    Rig rig;
    REQUIRE(rig.dd->SetDisplayMode(800, 600, 16, 0, 0) == kDdOk);
    giveWindow(rig, 2560, 1440, 8192);
    rig.gpu.backBufferResult = Error::kUnsupported;
    rig.gpu.log.Clear();
    CHECK(rig.dd->BeginSceneScale() == 1);
    CHECK(rig.gpu.log.Count("ConfigureScaledBackBuffer") == 1);
    CHECK(rig.dd->WantedRenderScale() == 1);
    rig.gpu.backBufferResult = platform::TextureId{4};
    CHECK(rig.dd->BeginSceneScale() == 1);
    CHECK(rig.gpu.log.Count("ConfigureScaledBackBuffer") == 1);
    REQUIRE(rig.dd->DisplayTexture().Ok()); // made again at 1
    CHECK(rig.gpu.log.Calls().back() == "ConfigureBackBuffer(800, 600, 0)");

    REQUIRE(rig.dd->SetDisplayMode(800, 600, 16, 0, 0) == kDdOk);
    CHECK(rig.dd->WantedRenderScale() == 3); // a new mode asks again
}

} // namespace sfcop::hle::ddraw4
