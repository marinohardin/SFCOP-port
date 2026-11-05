// What the gdi32 doctests share: a Gdi on the host heap with counting handles and a 640x480,
// 32-bit screen, and 8x8 bitmaps written by hand.
#pragma once

#include "hle/gdi32/Gdi.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>

namespace sfcop::hle::gdi32::test {

struct Rig {
    HeapPixelMemory memory;
    Gdi gdi{memory, countingHandles(), [] { return ScreenInfo{640, 480, 32, 60}; }};

    // A memory DC with a new 8x8 DIB section of `bits` bits selected, top-down.
    std::uint32_t Dc8x8(std::uint16_t bits = 32, std::uint32_t *bitmap = nullptr) {
        const std::uint32_t dc = gdi.CreateCompatibleDC(0);
        BitmapFormat format;
        format.width = 8;
        format.height = -8;
        format.bitCount = bits;
        if (bits == 8) {
            format.colorCount = 4;
            format.colors[0] = 0x000000;
            format.colors[1] = 0xFF0000;
            format.colors[2] = 0x00FF00;
            format.colors[3] = 0xFFFFFF;
        }
        const std::uint32_t made = gdi.CreateDIBSection(dc, format, nullptr);
        REQUIRE(made != 0);
        REQUIRE(gdi.SelectObject(dc, made) != 0);
        if (bitmap != nullptr) {
            *bitmap = made;
        }
        return dc;
    }

    // Row `y` of the DC's bitmap as one character a pixel: '.' black, '#' white, 'r' red, 'g'
    // green, 'b' blue, '?' anything else.
    std::string Row(std::uint32_t dc, std::int32_t y) {
        const Bitmap *bitmap = gdi.SelectedBitmap(dc);
        std::string out;
        for (std::int32_t x = 0; x < bitmap->Width(); ++x) {
            switch (bitmap->Get(x, y)) {
            case 0x000000:
                out += '.';
                break;
            case 0xFFFFFF:
                out += '#';
                break;
            case 0xFF0000:
                out += 'r';
                break;
            case 0x00FF00:
                out += 'g';
                break;
            case 0x0000FF:
                out += 'b';
                break;
            default:
                out += '?';
                break;
            }
        }
        return out;
    }
};

} // namespace sfcop::hle::gdi32::test
