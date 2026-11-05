#include "hle/gdi32/GdiTestRig.h"

namespace sfcop::hle::gdi32 {

using test::Rig;

TEST_CASE("CreateCompatibleDC makes a DC that DeleteDC removes, and refuses a foreign DC") {
    Rig rig;
    const std::uint32_t dc = rig.gdi.CreateCompatibleDC(0);
    CHECK(dc != 0);
    CHECK(rig.gdi.OwnsDc(dc));
    CHECK(rig.gdi.CreateCompatibleDC(0x7777) == 0);
    CHECK(rig.gdi.DeleteDC(dc));
    CHECK_FALSE(rig.gdi.OwnsDc(dc));
    CHECK_FALSE(rig.gdi.DeleteDC(dc));
}

TEST_CASE("GetDeviceCaps reports the game's screen for every DC") {
    Rig rig;
    const std::uint32_t screen = rig.gdi.DcOnBitmap(0);
    const std::uint32_t memoryDc = rig.gdi.CreateCompatibleDC(screen);
    for (const std::uint32_t dc : {screen, memoryDc}) {
        CHECK(rig.gdi.GetDeviceCaps(dc, kHorzRes) == 640);
        CHECK(rig.gdi.GetDeviceCaps(dc, kVertRes) == 480);
        CHECK(rig.gdi.GetDeviceCaps(dc, kBitsPixel) == 32);
        CHECK(rig.gdi.GetDeviceCaps(dc, kPlanes) == 1);
        CHECK(rig.gdi.GetDeviceCaps(dc, kLogPixelsY) == 96);
        CHECK(rig.gdi.GetDeviceCaps(dc, kSizePalette) == 0);
        CHECK(rig.gdi.GetDeviceCaps(dc, kNumColors) == 0xFFFFFFFFu);
        CHECK(rig.gdi.GetDeviceCaps(dc, kVRefresh) == 60);
    }
    CHECK(rig.gdi.GetDeviceCaps(0x7777, kHorzRes) == 0);
}

TEST_CASE("CreateDIBSection hands out the bits the bitmap draws from") {
    Rig rig;
    BitmapFormat format;
    format.width = 3;
    format.height = 2; // bottom-up
    format.bitCount = 24;
    std::uint8_t *bits = nullptr;
    const std::uint32_t bitmap = rig.gdi.CreateDIBSection(0, format, &bits);
    REQUIRE(bitmap != 0);
    REQUIRE(bits != nullptr);
    // Rows are padded to four bytes (12 bytes for 3 pixels of 3 bytes); the last row in memory is
    // the top one. Pixel (0, 0) is the first of the second stored row: blue, green, red.
    bits[12] = 0x11, bits[13] = 0x22, bits[14] = 0x33;
    CHECK(rig.gdi.BitmapOf(bitmap)->Get(0, 0) == 0x332211u);
    CHECK(rig.gdi.BitmapOf(bitmap)->Pitch() == 12);
}

TEST_CASE("CreateDIBSection refuses a format it does not draw") {
    Rig rig;
    BitmapFormat format;
    format.width = 8;
    format.height = 8;
    format.bitCount = 4;
    std::uint8_t *bits = reinterpret_cast<std::uint8_t *>(1);
    CHECK(rig.gdi.CreateDIBSection(0, format, &bits) == 0);
    format.bitCount = 16;
    format.compression = 1; // BI_RLE8
    CHECK(rig.gdi.CreateDIBSection(0, format, nullptr) == 0);
    format.compression = 0;
    format.width = 0;
    CHECK(rig.gdi.CreateDIBSection(0, format, nullptr) == 0);
}

TEST_CASE("SelectObject returns what it replaced and refuses a bitmap selected elsewhere") {
    Rig rig;
    std::uint32_t bitmap = 0;
    const std::uint32_t dc = rig.Dc8x8(32, &bitmap);
    const std::uint32_t other = rig.gdi.CreateCompatibleDC(0);
    CHECK(rig.gdi.SelectObject(other, bitmap) == kHgdiError);
    const std::uint32_t font = rig.gdi.CreateFont(FontDesc{16, 700, false, false, "Arial"});
    const std::uint32_t systemFont = rig.gdi.GetStockObject(kSystemFont);
    CHECK(rig.gdi.SelectObject(dc, font) == systemFont);
    CHECK(rig.gdi.SelectObject(dc, systemFont) == font);
    CHECK(rig.gdi.SelectObject(dc, 0x7777) == 0);
}

TEST_CASE("GetStockObject gives one handle per stock object, which DeleteObject keeps") {
    Rig rig;
    const std::uint32_t black = rig.gdi.GetStockObject(kBlackBrush);
    CHECK(black != 0);
    CHECK(rig.gdi.GetStockObject(kBlackBrush) == black);
    CHECK(rig.gdi.GetStockObject(kWhiteBrush) != black);
    CHECK(rig.gdi.GetStockObject(9) == 0); // no such stock object
    CHECK(rig.gdi.GetStockObject(kStockObjectCount) == 0);
    CHECK(rig.gdi.DeleteObject(black));
    CHECK(rig.gdi.OwnsObject(black));
}

TEST_CASE("DeleteObject of a selected bitmap waits for the DC to let it go") {
    Rig rig;
    std::uint32_t bitmap = 0;
    const std::uint32_t dc = rig.Dc8x8(32, &bitmap);
    CHECK(rig.gdi.DeleteObject(bitmap));
    CHECK(rig.gdi.OwnsObject(bitmap));
    CHECK(rig.gdi.DeleteDC(dc));
    CHECK_FALSE(rig.gdi.OwnsObject(bitmap));
    CHECK_FALSE(rig.gdi.DeleteObject(bitmap));
}

TEST_CASE("The DIB colour table of the selected bitmap is read and written by entry") {
    Rig rig;
    const std::uint32_t dc = rig.Dc8x8(8);
    Quad table[4];
    CHECK(rig.gdi.GetDIBColorTable(dc, 0, 4, table) == 4);
    CHECK(table[1].red == 0xFF);
    CHECK(table[1].green == 0);
    CHECK(table[2].green == 0xFF);
    const Quad blue{0xFF, 0, 0, 0};
    CHECK(rig.gdi.SetDIBColorTable(dc, 1, 1, &blue) == 1);
    CHECK(rig.gdi.GetDIBColorTable(dc, 1, 1, table) == 1);
    CHECK(table[0].blue == 0xFF);
    CHECK(table[0].red == 0);
    CHECK(rig.gdi.GetDIBColorTable(rig.Dc8x8(32), 0, 4, table) == 0); // not a palette bitmap
}

TEST_CASE("GetSystemPaletteEntries gives the static colours at both ends") {
    Rig rig;
    const std::uint32_t dc = rig.gdi.DcOnBitmap(0);
    Quad entries[256];
    CHECK(rig.gdi.GetSystemPaletteEntries(dc, 0, 256, entries) == 256);
    CHECK(entries[1].red == 0x80);
    CHECK(entries[1].green == 0);
    CHECK(entries[100].red == 0);
    CHECK(entries[255].red == 0xFF);
    CHECK(entries[255].green == 0xFF);
    CHECK(entries[255].blue == 0xFF);
    CHECK(rig.gdi.GetSystemPaletteEntries(dc, 250, 10, entries) == 6);
    CHECK(rig.gdi.GetSystemPaletteEntries(0x7777, 0, 1, entries) == 0);
}

} // namespace sfcop::hle::gdi32
