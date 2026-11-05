#include "hle/gdi32/GdiTestRig.h"

namespace sfcop::hle::gdi32 {

using test::Rig;

namespace {

void paint(Rig &rig, std::uint32_t dc, std::int32_t left, std::int32_t top, std::int32_t right,
           std::int32_t bottom, Colorref color) {
    REQUIRE(rig.gdi.FillSolid(dc, Rect{left, top, right, bottom}, color));
}

} // namespace

TEST_CASE("BitBlt SRCCOPY copies a block, clipped at the destination's edge") {
    Rig rig;
    const std::uint32_t src = rig.Dc8x8(), dst = rig.Dc8x8();
    paint(rig, src, 0, 0, 2, 2, rgb(255, 0, 0));
    CHECK(rig.gdi.BitBlt(dst, 6, 1, 4, 3, src, 0, 0, kSrcCopy));
    CHECK(rig.Row(dst, 0) == "........");
    CHECK(rig.Row(dst, 1) == "......rr");
    CHECK(rig.Row(dst, 2) == "......rr");
    CHECK(rig.Row(dst, 3) == "........");
}

TEST_CASE("BitBlt converts between bit depths") {
    Rig rig;
    const std::uint32_t src = rig.Dc8x8(8), dst16 = rig.Dc8x8(16), dst24 = rig.Dc8x8(24);
    paint(rig, src, 0, 0, 8, 1, rgb(0, 255, 0)); // index 2
    CHECK(rig.gdi.SelectedBitmap(src)->Row(0)[0] == 2);
    CHECK(rig.gdi.BitBlt(dst16, 0, 0, 8, 1, src, 0, 0, kSrcCopy));
    CHECK(rig.gdi.BitBlt(dst24, 0, 0, 8, 1, src, 0, 0, kSrcCopy));
    CHECK(rig.Row(dst16, 0) == "gggggggg");
    CHECK(rig.Row(dst24, 0) == "gggggggg");
    // 5-5-5 green, little-endian: 0x03E0.
    CHECK(rig.gdi.SelectedBitmap(dst16)->Row(0)[0] == 0xE0);
    CHECK(rig.gdi.SelectedBitmap(dst16)->Row(0)[1] == 0x03);
}

TEST_CASE("The raster operations combine source, destination and pattern as their tables say") {
    Rig rig;
    const std::uint32_t src = rig.Dc8x8(), dst = rig.Dc8x8();
    paint(rig, src, 0, 0, 4, 1, rgb(255, 255, 255)); // source row: ####....
    paint(rig, dst, 2, 0, 6, 1, rgb(255, 0, 0));     // dest row:   ..rrrr..

    SUBCASE("SRCPAINT ors") {
        CHECK(rig.gdi.BitBlt(dst, 0, 0, 8, 1, src, 0, 0, kSrcPaint));
        CHECK(rig.Row(dst, 0) == "####rr..");
    }
    SUBCASE("SRCAND ands") {
        CHECK(rig.gdi.BitBlt(dst, 0, 0, 8, 1, src, 0, 0, kSrcAnd));
        CHECK(rig.Row(dst, 0) == "..rr....");
    }
    SUBCASE("SRCINVERT xors") {
        CHECK(rig.gdi.BitBlt(dst, 0, 0, 8, 1, src, 0, 0, kSrcInvert));
        CHECK(rig.Row(dst, 0) == "##??rr.."); // white xor red is cyan
    }
    SUBCASE("NOTSRCCOPY inverts the source") {
        CHECK(rig.gdi.BitBlt(dst, 0, 0, 8, 1, src, 0, 0, kNotSrcCopy));
        CHECK(rig.Row(dst, 0) == "....####");
    }
    SUBCASE("PATCOPY paints the brush, BLACKNESS and WHITENESS ignore it") {
        rig.gdi.SelectObject(dst, rig.gdi.GetStockObject(kWhiteBrush));
        CHECK(rig.gdi.PatBlt(dst, 0, 0, 2, 1, kPatCopy));
        CHECK(rig.gdi.PatBlt(dst, 2, 0, 2, 1, kBlackness));
        CHECK(rig.gdi.PatBlt(dst, 6, 0, 2, 1, kWhiteness));
        CHECK(rig.Row(dst, 0) == "##..rr##");
    }
    SUBCASE("DSTINVERT inverts the destination") {
        CHECK(rig.gdi.PatBlt(dst, 0, 0, 8, 1, kDstInvert));
        CHECK(rig.Row(dst, 0) == "##????##");
    }
    SUBCASE("PatBlt refuses an operation that reads a source") {
        CHECK_FALSE(rig.gdi.PatBlt(dst, 0, 0, 8, 1, kSrcCopy));
    }
}

TEST_CASE("StretchBlt scales by nearest pixel and mirrors a negative width") {
    Rig rig;
    const std::uint32_t src = rig.Dc8x8(), dst = rig.Dc8x8();
    paint(rig, src, 0, 0, 1, 1, rgb(255, 0, 0));
    paint(rig, src, 1, 0, 2, 1, rgb(0, 0, 255));
    CHECK(rig.gdi.StretchBlt(dst, 0, 0, 8, 2, src, 0, 0, 2, 1, kSrcCopy));
    CHECK(rig.Row(dst, 0) == "rrrrbbbb");
    CHECK(rig.Row(dst, 1) == "rrrrbbbb");
    CHECK(rig.gdi.StretchBlt(dst, 0, 2, 4, 1, src, 1, 0, -2, 1, kSrcCopy));
    CHECK(rig.Row(dst, 2) == "bbrr....");
}

TEST_CASE("FillRect takes a brush or a system colour index + 1, and fails for a foreign DC") {
    Rig rig;
    const std::uint32_t dc = rig.Dc8x8();
    CHECK(rig.gdi.FillRect(dc, Rect{0, 0, 3, 1}, rig.gdi.GetStockObject(kWhiteBrush)));
    CHECK(rig.gdi.FillRect(dc, Rect{3, 0, 5, 1}, 6)); // COLOR_WINDOW + 1: white
    CHECK(rig.gdi.FillRect(dc, Rect{5, 0, 8, 1}, rig.gdi.GetStockObject(kNullBrush)));
    CHECK(rig.Row(dc, 0) == "#####...");
    CHECK_FALSE(rig.gdi.FillRect(0x7777, Rect{0, 0, 1, 1}, 6));
    CHECK_FALSE(rig.gdi.FillRect(dc, Rect{0, 0, 1, 1}, 0x7777));
}

TEST_CASE("A blit from or to a foreign DC fails, so the guest layer can hand it on") {
    Rig rig;
    const std::uint32_t dc = rig.Dc8x8();
    CHECK_FALSE(rig.gdi.BitBlt(0x7777, 0, 0, 1, 1, dc, 0, 0, kSrcCopy));
    CHECK_FALSE(rig.gdi.BitBlt(dc, 0, 0, 1, 1, 0x7777, 0, 0, kSrcCopy));
}

} // namespace sfcop::hle::gdi32
