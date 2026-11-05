#include "hle/gdi32/GdiTestRig.h"

namespace sfcop::hle::gdi32 {

using test::Rig;

TEST_CASE("TextOut draws a glyph of the built-in font in the text colour") {
    Rig rig;
    const std::uint32_t dc = rig.Dc8x8();
    rig.gdi.SetTextColor(dc, rgb(255, 255, 255));
    rig.gdi.SetBkMode(dc, kTransparent);
    CHECK(rig.gdi.TextOut(dc, 1, 0, "T"));
    CHECK(rig.Row(dc, 0) == ".#####..");
    CHECK(rig.Row(dc, 1) == "...#....");
    CHECK(rig.Row(dc, 6) == "...#....");
    CHECK(rig.Row(dc, 7) == "........");
}

TEST_CASE("An opaque background fills the cell; a transparent one leaves it") {
    Rig rig;
    const std::uint32_t dc = rig.Dc8x8();
    rig.gdi.SetTextColor(dc, rgb(255, 255, 255));
    rig.gdi.SetBkColor(dc, rgb(0, 0, 255));
    CHECK(rig.gdi.SetBkMode(dc, kOpaque) == kOpaque); // the default
    CHECK(rig.gdi.TextOut(dc, 0, 0, "-"));
    CHECK(rig.Row(dc, 2) == "bbbbbb..");
    CHECK(rig.Row(dc, 3) == "#####b..");
    CHECK(rig.gdi.SetBkMode(dc, 7) == 0);
}

TEST_CASE("SetTextAlign right and baseline move the text's anchor") {
    Rig rig;
    const std::uint32_t dc = rig.Dc8x8();
    rig.gdi.SetTextColor(dc, rgb(255, 255, 255));
    rig.gdi.SetBkMode(dc, kTransparent);
    CHECK(rig.gdi.SetTextAlign(dc, kTaRight | kTaBaseline) == 0);
    CHECK(rig.gdi.TextOut(dc, 7, 7, "_")); // the cell is x 1-6, y 0-7; '_' is its last glyph row
    CHECK(rig.Row(dc, 6) == ".#####..");
}

TEST_CASE("ExtTextOut ETO_OPAQUE fills its rectangle and ETO_CLIPPED clips to it") {
    Rig rig;
    const std::uint32_t dc = rig.Dc8x8();
    rig.gdi.SetTextColor(dc, rgb(255, 255, 255));
    rig.gdi.SetBkColor(dc, rgb(255, 0, 0));
    rig.gdi.SetBkMode(dc, kTransparent);
    const Rect box{0, 0, 3, 8};
    CHECK(rig.gdi.ExtTextOut(dc, 0, 0, kEtoOpaque | kEtoClipped, &box, "T", nullptr));
    CHECK(rig.Row(dc, 0) == "###.....");
    CHECK(rig.Row(dc, 1) == "rr#.....");
}

TEST_CASE("Extents and metrics come from the 6x8 cell at the font's scale") {
    Rig rig;
    const std::uint32_t dc = rig.Dc8x8();
    Size size;
    CHECK(rig.gdi.GetTextExtent(dc, "Hello", size));
    CHECK(size.cx == 30);
    CHECK(size.cy == 8);
    const std::uint32_t big = rig.gdi.CreateFont(FontDesc{-16, 400, false, false, "Arial"});
    rig.gdi.SelectObject(dc, big);
    CHECK(rig.gdi.GetTextExtent(dc, "Hi", size));
    CHECK(size.cx == 24);
    CHECK(size.cy == 16);
    TextMetrics metrics{};
    CHECK(rig.gdi.GetTextMetrics(dc, metrics));
    CHECK(metrics.height == 16);
    CHECK(metrics.ascent == 14);
    CHECK(metrics.descent == 2);
    CHECK(metrics.aveCharWidth == 12);
    CHECK(metrics.firstChar == 0x20);
    CHECK(metrics.lastChar == 0x7E);
    CHECK_FALSE(rig.gdi.GetTextMetrics(0x7777, metrics));
}

TEST_CASE("fontScale rounds the requested height to whole cells, at least one") {
    CHECK(fontScale(0) == 1);
    CHECK(fontScale(-11) == 1);
    CHECK(fontScale(12) == 2);
    CHECK(fontScale(-24) == 3);
}

TEST_CASE("A text colour or background mode set on a foreign DC reports failure") {
    Rig rig;
    CHECK(rig.gdi.SetTextColor(0x7777, 0) == kClrInvalid);
    CHECK(rig.gdi.SetBkColor(0x7777, 0) == kClrInvalid);
    CHECK(rig.gdi.SetTextAlign(0x7777, 0) == 0xFFFFFFFFu);
    CHECK_FALSE(rig.gdi.TextOut(0x7777, 0, 0, "x"));
}

} // namespace sfcop::hle::gdi32
