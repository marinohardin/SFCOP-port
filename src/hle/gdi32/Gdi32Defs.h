// The GDI constants the module uses, with the values of the Win32 headers. Structures are not
// here: the guest shims read and write them field by field (Gdi32Guest.cpp).
#pragma once

#include <cstdint>

namespace sfcop::hle::gdi32 {

// A Win32 COLORREF: 0x00BBGGRR.
using Colorref = std::uint32_t;

// Raster operations (BitBlt, StretchBlt, PatBlt). Bits 16-23 are the ROP3 truth table.
constexpr std::uint32_t kSrcCopy = 0x00CC0020u;
constexpr std::uint32_t kSrcPaint = 0x00EE0086u;
constexpr std::uint32_t kSrcAnd = 0x008800C6u;
constexpr std::uint32_t kSrcInvert = 0x00660046u;
constexpr std::uint32_t kNotSrcCopy = 0x00330008u;
constexpr std::uint32_t kPatCopy = 0x00F00021u;
constexpr std::uint32_t kPatInvert = 0x005A0049u;
constexpr std::uint32_t kDstInvert = 0x00550009u;
constexpr std::uint32_t kBlackness = 0x00000042u;
constexpr std::uint32_t kWhiteness = 0x00FF0062u;

// GetStockObject indices.
enum StockObject : std::uint32_t {
    kWhiteBrush = 0,
    kLtGrayBrush = 1,
    kGrayBrush = 2,
    kDkGrayBrush = 3,
    kBlackBrush = 4,
    kNullBrush = 5,
    kWhitePen = 6,
    kBlackPen = 7,
    kNullPen = 8,
    kOemFixedFont = 10,
    kAnsiFixedFont = 11,
    kAnsiVarFont = 12,
    kSystemFont = 13,
    kDeviceDefaultFont = 14,
    kDefaultPalette = 15,
    kSystemFixedFont = 16,
    kDefaultGuiFont = 17,
    kDcBrush = 18,
    kDcPen = 19,
    kStockObjectCount = 20,
};

// GetDeviceCaps indices.
constexpr std::uint32_t kDriverVersion = 0;
constexpr std::uint32_t kTechnology = 2;
constexpr std::uint32_t kHorzSize = 4;
constexpr std::uint32_t kVertSize = 6;
constexpr std::uint32_t kHorzRes = 8;
constexpr std::uint32_t kVertRes = 10;
constexpr std::uint32_t kBitsPixel = 12;
constexpr std::uint32_t kPlanes = 14;
constexpr std::uint32_t kNumColors = 24;
constexpr std::uint32_t kRasterCaps = 38;
constexpr std::uint32_t kLogPixelsX = 88;
constexpr std::uint32_t kLogPixelsY = 90;
constexpr std::uint32_t kSizePalette = 104;
constexpr std::uint32_t kNumReserved = 106;
constexpr std::uint32_t kColorRes = 108;
constexpr std::uint32_t kVRefresh = 116;

constexpr std::uint32_t kDtRasDisplay = 1;
// RC_BITBLT | RC_BITMAP64 | RC_DI_BITMAP | RC_DIBTODEV | RC_STRETCHBLT | RC_STRETCHDIB
constexpr std::uint32_t kRasterCapsValue = 0x0001u | 0x0008u | 0x0080u | 0x0200u | 0x0800u | 0x2000u;

// Background modes.
constexpr std::uint32_t kTransparent = 1;
constexpr std::uint32_t kOpaque = 2;

// SetTextAlign flags.
constexpr std::uint32_t kTaUpdateCp = 1;
constexpr std::uint32_t kTaRight = 2;
constexpr std::uint32_t kTaCenter = 6;
constexpr std::uint32_t kTaBottom = 8;
constexpr std::uint32_t kTaBaseline = 24;

// ExtTextOut options.
constexpr std::uint32_t kEtoOpaque = 2;
constexpr std::uint32_t kEtoClipped = 4;

// BITMAPINFOHEADER compression.
constexpr std::uint32_t kBiRgb = 0;
constexpr std::uint32_t kBiBitfields = 3;

// CreateDIBSection usage.
constexpr std::uint32_t kDibRgbColors = 0;
constexpr std::uint32_t kDibPalColors = 1;

// What GDI returns for a failed SetTextColor/SetBkColor and a failed SetBkMode.
constexpr Colorref kClrInvalid = 0xFFFFFFFFu;
// What SelectObject returns for a failure (HGDI_ERROR).
constexpr std::uint32_t kHgdiError = 0xFFFFFFFFu;

// A COLORREF from its channels, and back.
constexpr Colorref rgb(std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    return static_cast<Colorref>(red) | static_cast<Colorref>(green) << 8 |
           static_cast<Colorref>(blue) << 16;
}

// This module's pixels: 0x00RRGGBB, the order of a 32-bit DIB in memory read as a word.
constexpr std::uint32_t pixelOf(Colorref color) {
    return (color & 0xFFu) << 16 | (color & 0xFF00u) | (color >> 16 & 0xFFu);
}
constexpr Colorref colorrefOf(std::uint32_t pixel) {
    return (pixel >> 16 & 0xFFu) | (pixel & 0xFF00u) | (pixel & 0xFFu) << 16;
}

} // namespace sfcop::hle::gdi32
