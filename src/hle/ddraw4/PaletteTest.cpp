#include "hle/ddraw4/Palette.h"
#include "hle/ddraw4/Ddraw4TestRig.h"

#include <array>

namespace sfcop::hle::ddraw4 {

using platform::PixelFormat;
using platform::TextureId;
using test::Calls;
using test::Rig;

namespace {

std::array<PaletteColor, 256> grey() {
    std::array<PaletteColor, 256> colors{};
    for (std::size_t i = 0; i < colors.size(); ++i) {
        const auto level = static_cast<std::uint8_t>(i);
        colors[i] = PaletteColor{level, level, level, 0};
    }
    return colors;
}

} // namespace

TEST_CASE("a palette keeps the colours it was made with and its caps") {
    Rig rig;
    const std::array<PaletteColor, 256> colors = grey();
    Palette *palette = nullptr;
    REQUIRE(rig.dd->CreatePalette(kDdpcaps8Bit | kDdpcapsAllow256, colors.data(), &palette) ==
            kDdOk);
    std::uint32_t caps = 0;
    CHECK(palette->GetCaps(caps) == kDdOk);
    CHECK(caps == 0x44u);
    std::array<PaletteColor, 2> out{};
    CHECK(palette->GetEntries(0, 254, 2, out.data()) == kDdOk);
    CHECK(out[0].red == 254);
    CHECK(out[1].blue == 255);
    CHECK(palette->GetEntries(0, 255, 2, out.data()) == kDdErrInvalidParams);
    CHECK(palette->Initialize() == kDdErrAlreadyInitialized);
    palette->Release();
}

TEST_CASE("SetEntries replaces colours and writes them to the Gpu palette once there is one") {
    Rig rig;
    const std::array<PaletteColor, 256> colors = grey();
    Palette *palette = nullptr;
    REQUIRE(rig.dd->CreatePalette(kDdpcaps8Bit, colors.data(), &palette) == kDdOk);
    const PaletteColor red[2] = {{255, 0, 0, 0}, {200, 0, 0, 0}};
    CHECK(palette->SetEntries(0, 10, 2, red) == kDdOk);
    CHECK(rig.gpu.log.Calls().empty());
    std::array<PaletteColor, 1> out{};
    CHECK(palette->GetEntries(0, 11, 1, out.data()) == kDdOk);
    CHECK(out[0].red == 200);
    CHECK(out[0].green == 0);

    REQUIRE(palette->GpuPalette().Ok());
    rig.gpu.log.Clear();
    CHECK(palette->SetEntries(0, 10, 2, red) == kDdOk);
    CHECK(rig.gpu.log.Calls() == Calls{"WritePalette(1, 10, 2)"});
    CHECK(palette->SetEntries(0, 255, 2, red) == kDdErrInvalidParams);
    CHECK(palette->SetEntries(0, 0, 0, red) == kDdErrInvalidParams);
    palette->Release();
    CHECK(rig.gpu.log.Calls().back() == "DestroyPalette(1)");
}

TEST_CASE("SetEntries on the 8-bit primary's palette shows the primary again") {
    Rig rig;
    REQUIRE(rig.dd->SetDisplayMode(640, 480, 8, 0, 0) == kDdOk);
    Surface *primary = rig.Create(test::primaryDesc());
    const std::array<PaletteColor, 256> colors = grey();
    Palette *palette = nullptr;
    REQUIRE(rig.dd->CreatePalette(kDdpcaps8Bit, colors.data(), &palette) == kDdOk);
    rig.gpu.createTextureResult = TextureId{5};
    REQUIRE(primary->SetPalette(palette) == kDdOk);
    rig.gpu.log.Clear();
    const PaletteColor blue{0, 0, 255, 0};
    CHECK(palette->SetEntries(0, 1, 1, &blue) == kDdOk);
    CHECK(rig.gpu.log.Calls() == Calls{"WritePalette(1, 1, 1)", "Blit(5, 1, false)", "Present()"});
    palette->Release();
    primary->Release();
}

TEST_CASE("a palette keeps its DirectDraw object alive") {
    Rig rig;
    const std::array<PaletteColor, 256> colors = grey();
    Palette *palette = nullptr;
    REQUIRE(rig.dd->CreatePalette(kDdpcaps8Bit, colors.data(), &palette) == kDdOk);
    CHECK(rig.dd->Release() == 0);
    CHECK_FALSE(test::wasDestroyed(rig.dd));
    palette->Release();
    CHECK(test::wasDestroyed(rig.dd));
}

} // namespace sfcop::hle::ddraw4
