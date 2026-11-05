// The one font this module draws with. The native build has no GDI fonts, so every font the game
// creates is a fixed-cell ASCII font, a 5x7 bitmap glyph in a 6x8 cell, drawn at an
// integer scale chosen from the height the game asked for. Metrics and extents come from it.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace sfcop::hle::gdi32 {

constexpr std::int32_t kCellWidth = 6;
constexpr std::int32_t kCellHeight = 8;
constexpr std::int32_t kGlyphRows = 7;

// The rows of a character's glyph, top first; bit 4 of a row is its leftmost pixel. A character
// outside 0x20-0x7E has a box.
const std::uint8_t *glyphRows(char c);

// What CreateFontA asked for: the parts this font honours.
struct FontDesc {
    std::int32_t height = 0; // logical units; negative: the character height, 0: the default
    std::int32_t weight = 400;
    bool italic = false;
    bool underline = false;
    std::string face;
};

// The scale a requested height gives: |height| / 8 rounded, at least 1.
std::int32_t fontScale(std::int32_t height);

struct TextMetrics {
    std::int32_t height, ascent, descent, internalLeading, externalLeading;
    std::int32_t aveCharWidth, maxCharWidth, weight, overhang;
    std::uint8_t firstChar, lastChar, defaultChar, breakChar;
    std::uint8_t italic, underlined, struckOut, pitchAndFamily, charSet;
};

TextMetrics fontMetrics(const FontDesc &font);

} // namespace sfcop::hle::gdi32
