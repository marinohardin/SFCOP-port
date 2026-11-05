// The guest shims of the gdi32 module (src/hle/README.md, "Implementing a module"): the GDI32
// rows of src/hle/surface.tsv. Each reads its arguments from the guest stack, reads and writes
// the game's structures field by field, calls the module (Gdi.h) and purges the row's stack
// bytes. The shared state (the one Gdi, its guest memory and handles) is GuestGdi.h's.
//
// A row given a DC this module did not make (a DirectDraw surface's, which win32hle makes) runs
// win32hle's own row of that name.
#include "hle/gdi32/Gdi.h"
#include "hle/gdi32/Gdi32Defs.h"
#include "hle/gdi32/GuestGdi.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace sfcop::hle::gdi32 {

namespace {

using guest::foreignDc;
using guest::gdi;

void handOn(const char *name, std::uint32_t argc, std::uint32_t failure) {
    if (recomp_func_t row = guest::win32hleRow(name)) {
        row();
        return;
    }
    RET(failure, argc);
}

void put32(std::uint32_t address, std::uint32_t value) {
    if (address != 0) {
        MEM32(address) = value;
    }
}

Rect readRect(std::uint32_t address) {
    return Rect{static_cast<std::int32_t>(MEM32(address)),
                static_cast<std::int32_t>(MEM32(address + 4)),
                static_cast<std::int32_t>(MEM32(address + 8)),
                static_cast<std::int32_t>(MEM32(address + 12))};
}

std::string_view guestText(std::uint32_t address, std::uint32_t count) {
    if (address == 0) {
        return {};
    }
    return std::string_view(static_cast<const char *>(hle_ptr(address)), count);
}

std::int32_t s32(std::uint32_t value) {
    return static_cast<std::int32_t>(value);
}

// ---- device contexts and objects ----

void g_CreateCompatibleDC() { // (hdc)
    RET(gdi().CreateCompatibleDC(A32(0)), 1);
}

void g_DeleteDC() { // (hdc)
    if (foreignDc(A32(0))) {
        return handOn("DeleteDC", 1, 0);
    }
    RET(gdi().DeleteDC(A32(0)) ? 1 : 0, 1);
}

void g_GetDeviceCaps() { // (hdc, index)
    RET(gdi().GetDeviceCaps(A32(0), A32(1)), 2);
}

// (hdc, const BITMAPINFO *, usage, void **bits, hSection, offset)
void g_CreateDIBSection() {
    const std::uint32_t info = A32(1);
    const std::uint32_t usage = A32(2);
    const std::uint32_t bitsOut = A32(3);
    put32(bitsOut, 0);
    if (info == 0 || A32(4) != 0) {
        RET(0, 6); // a file-mapping section is not supported
    }
    BitmapFormat format;
    const std::uint32_t headerSize = MEM32(info);
    format.width = s32(MEM32(info + 4));
    format.height = s32(MEM32(info + 8));
    format.bitCount = static_cast<std::uint16_t>(MEM32(info + 12) >> 16);
    format.compression = MEM32(info + 16);
    const std::uint32_t used = MEM32(info + 32);
    const std::uint32_t table = info + headerSize;
    if (format.compression == kBiBitfields) {
        format.redMask = MEM32(table);
        format.greenMask = MEM32(table + 4);
        format.blueMask = MEM32(table + 8);
    } else if (format.bitCount <= 8) {
        const std::uint32_t full = 1u << format.bitCount;
        format.colorCount = used == 0 || used > full ? full : used;
        for (std::uint32_t i = 0; i < format.colorCount; ++i) {
            if (usage == kDibPalColors) {
                // Indices into the DC's palette: the palette is the system's (grey ramp).
                const std::uint32_t index = MEM8(table + 2 * i);
                format.colors[i] = index * 0x010101u;
            } else {
                const std::uint32_t quad = MEM32(table + 4 * i); // blue, green, red, reserved
                format.colors[i] = quad & 0x00FFFFFFu;
            }
        }
    }
    std::uint8_t *bits = nullptr;
    const std::uint32_t bitmap = gdi().CreateDIBSection(A32(0), format, &bits);
    if (bitmap != 0) {
        put32(bitsOut, hle_va(bits));
    }
    RET(bitmap, 6);
}

// (height, width, escapement, orientation, weight, italic, underline, strikeOut, charSet,
//  outPrecision, clipPrecision, quality, pitchAndFamily, faceName)
void g_CreateFontA() {
    FontDesc font;
    font.height = s32(A32(0));
    font.weight = s32(A32(4));
    font.italic = (A32(5) & 0xFF) != 0;
    font.underline = (A32(6) & 0xFF) != 0;
    if (A32(13) != 0) {
        // lfFaceName is LF_FACESIZE (32) bytes: no more is read, terminated or not.
        const char *face = static_cast<const char *>(hle_ptr(A32(13)));
        font.face.assign(face, strnlen(face, 31));
    }
    RET(gdi().CreateFont(font), 14);
}

void g_GetStockObject() { // (index)
    RET(gdi().GetStockObject(A32(0)), 1);
}

void g_SelectObject() { // (hdc, object)
    if (foreignDc(A32(0))) {
        return handOn("SelectObject", 2, 0);
    }
    RET(gdi().SelectObject(A32(0), A32(1)), 2);
}

void g_DeleteObject() { // (object)
    if (A32(0) != 0 && !gdi().OwnsObject(A32(0))) {
        return handOn("DeleteObject", 1, 0);
    }
    RET(gdi().DeleteObject(A32(0)) ? 1 : 0, 1);
}

// ---- drawing ----

// (hdcDest, x, y, width, height, hdcSrc, xSrc, ySrc, rop)
void g_BitBlt() {
    if (foreignDc(A32(0)) || foreignDc(A32(5))) {
        return handOn("BitBlt", 9, 0);
    }
    RET(gdi().BitBlt(A32(0), s32(A32(1)), s32(A32(2)), s32(A32(3)), s32(A32(4)), A32(5),
                     s32(A32(6)), s32(A32(7)), A32(8))
            ? 1
            : 0,
        9);
}

// (hdcDest, x, y, width, height, hdcSrc, xSrc, ySrc, widthSrc, heightSrc, rop)
void g_StretchBlt() {
    if (foreignDc(A32(0)) || foreignDc(A32(5))) {
        return handOn("StretchBlt", 11, 0);
    }
    RET(gdi().StretchBlt(A32(0), s32(A32(1)), s32(A32(2)), s32(A32(3)), s32(A32(4)), A32(5),
                         s32(A32(6)), s32(A32(7)), s32(A32(8)), s32(A32(9)), A32(10))
            ? 1
            : 0,
        11);
}

void g_PatBlt() { // (hdc, x, y, width, height, rop)
    if (foreignDc(A32(0))) {
        return handOn("PatBlt", 6, 0);
    }
    RET(gdi().PatBlt(A32(0), s32(A32(1)), s32(A32(2)), s32(A32(3)), s32(A32(4)), A32(5)) ? 1 : 0,
        6);
}

// ---- text ----

void g_SetTextColor() { // (hdc, color)
    if (foreignDc(A32(0))) {
        return handOn("SetTextColor", 2, kClrInvalid);
    }
    RET(gdi().SetTextColor(A32(0), A32(1)), 2);
}

void g_SetBkColor() { // (hdc, color)
    if (foreignDc(A32(0))) {
        return handOn("SetBkColor", 2, kClrInvalid);
    }
    RET(gdi().SetBkColor(A32(0), A32(1)), 2);
}

void g_SetBkMode() { // (hdc, mode)
    if (foreignDc(A32(0))) {
        return handOn("SetBkMode", 2, 0);
    }
    RET(gdi().SetBkMode(A32(0), A32(1)), 2);
}

void g_SetTextAlign() { // (hdc, align)
    if (foreignDc(A32(0))) {
        return handOn("SetTextAlign", 2, kHgdiError);
    }
    RET(gdi().SetTextAlign(A32(0), A32(1)), 2);
}

void g_TextOutA() { // (hdc, x, y, string, count)
    if (foreignDc(A32(0))) {
        return handOn("TextOutA", 5, 0);
    }
    RET(gdi().TextOut(A32(0), s32(A32(1)), s32(A32(2)), guestText(A32(3), A32(4))) ? 1 : 0, 5);
}

void g_ExtTextOutA() { // (hdc, x, y, options, const RECT *, string, count, const INT *dx)
    if (foreignDc(A32(0))) {
        return handOn("ExtTextOutA", 8, 0);
    }
    const std::uint32_t count = A32(6);
    Rect rect;
    const Rect *clip = nullptr;
    if (A32(4) != 0) {
        rect = readRect(A32(4));
        clip = &rect;
    }
    std::vector<std::int32_t> dx;
    if (A32(7) != 0) {
        for (std::uint32_t i = 0; i < count; ++i) {
            dx.push_back(s32(MEM32(A32(7) + 4 * i)));
        }
    }
    RET(gdi().ExtTextOut(A32(0), s32(A32(1)), s32(A32(2)), A32(3), clip, guestText(A32(5), count),
                         dx.empty() ? nullptr : dx.data())
            ? 1
            : 0,
        8);
}

void g_GetTextExtentPoint32A() { // (hdc, string, count, SIZE *)
    if (foreignDc(A32(0))) {
        return handOn("GetTextExtentPoint32A", 4, 0);
    }
    Size size;
    if (!gdi().GetTextExtent(A32(0), guestText(A32(1), A32(2)), size) || A32(3) == 0) {
        RET(0, 4);
    }
    put32(A32(3), static_cast<std::uint32_t>(size.cx));
    put32(A32(3) + 4, static_cast<std::uint32_t>(size.cy));
    RET(1, 4);
}

void g_GetTextMetricsA() { // (hdc, TEXTMETRICA *)
    if (foreignDc(A32(0))) {
        return handOn("GetTextMetricsA", 2, 0);
    }
    TextMetrics m{};
    const std::uint32_t out = A32(1);
    if (out == 0 || !gdi().GetTextMetrics(A32(0), m)) {
        RET(0, 2);
    }
    const std::int32_t longs[] = {m.height,       m.ascent,       m.descent,
                                  m.internalLeading, m.externalLeading, m.aveCharWidth,
                                  m.maxCharWidth, m.weight,       m.overhang,
                                  96,             96}; // tmDigitizedAspectX, Y
    for (std::uint32_t i = 0; i < 11; ++i) {
        put32(out + 4 * i, static_cast<std::uint32_t>(longs[i]));
    }
    const std::uint8_t bytes[] = {m.firstChar, m.lastChar,  m.defaultChar,   m.breakChar,
                                  m.italic,    m.underlined, m.struckOut, m.pitchAndFamily,
                                  m.charSet};
    for (std::uint32_t i = 0; i < 9; ++i) {
        MEM8(out + 44 + i) = bytes[i];
    }
    RET(1, 2);
}

// ---- palettes ----

void g_GetSystemPaletteEntries() { // (hdc, start, count, PALETTEENTRY *)
    std::vector<Quad> entries(A32(2));
    const std::uint32_t copied =
        gdi().GetSystemPaletteEntries(A32(0), A32(1), A32(2), entries.data());
    if (A32(3) == 0) {
        RET(256, 4); // without a buffer: the palette's size
    }
    for (std::uint32_t i = 0; i < copied; ++i) {
        // PALETTEENTRY is red, green, blue, flags.
        MEM8(A32(3) + 4 * i) = entries[i].red;
        MEM8(A32(3) + 4 * i + 1) = entries[i].green;
        MEM8(A32(3) + 4 * i + 2) = entries[i].blue;
        MEM8(A32(3) + 4 * i + 3) = 0;
    }
    RET(copied, 4);
}

void g_GetDIBColorTable() { // (hdc, start, count, RGBQUAD *)
    if (foreignDc(A32(0))) {
        return handOn("GetDIBColorTable", 4, 0);
    }
    std::vector<Quad> entries(A32(2));
    const std::uint32_t copied = gdi().GetDIBColorTable(A32(0), A32(1), A32(2), entries.data());
    for (std::uint32_t i = 0; i < copied && A32(3) != 0; ++i) {
        MEM8(A32(3) + 4 * i) = entries[i].blue;
        MEM8(A32(3) + 4 * i + 1) = entries[i].green;
        MEM8(A32(3) + 4 * i + 2) = entries[i].red;
        MEM8(A32(3) + 4 * i + 3) = 0;
    }
    RET(copied, 4);
}

void g_SetDIBColorTable() { // (hdc, start, count, const RGBQUAD *)
    if (foreignDc(A32(0))) {
        return handOn("SetDIBColorTable", 4, 0);
    }
    if (A32(3) == 0) {
        RET(0, 4);
    }
    std::vector<Quad> entries(A32(2));
    for (std::uint32_t i = 0; i < A32(2); ++i) {
        entries[i] = Quad{MEM8(A32(3) + 4 * i), MEM8(A32(3) + 4 * i + 1), MEM8(A32(3) + 4 * i + 2),
                          0};
    }
    RET(gdi().SetDIBColorTable(A32(0), A32(1), A32(2), entries.data()), 4);
}

} // namespace

} // namespace sfcop::hle::gdi32

namespace g = sfcop::hle::gdi32;

extern "C" const win32hle_shim sfcop_hle_gdi32[] = {
    {"CreateCompatibleDC", g::g_CreateCompatibleDC},
    {"DeleteDC", g::g_DeleteDC},
    {"GetDeviceCaps", g::g_GetDeviceCaps},
    {"CreateDIBSection", g::g_CreateDIBSection},
    {"CreateFontA", g::g_CreateFontA},
    {"GetStockObject", g::g_GetStockObject},
    {"SelectObject", g::g_SelectObject},
    {"DeleteObject", g::g_DeleteObject},
    {"BitBlt", g::g_BitBlt},
    {"StretchBlt", g::g_StretchBlt},
    {"PatBlt", g::g_PatBlt},
    {"SetTextColor", g::g_SetTextColor},
    {"SetBkColor", g::g_SetBkColor},
    {"SetBkMode", g::g_SetBkMode},
    {"SetTextAlign", g::g_SetTextAlign},
    {"TextOutA", g::g_TextOutA},
    {"ExtTextOutA", g::g_ExtTextOutA},
    {"GetTextExtentPoint32A", g::g_GetTextExtentPoint32A},
    {"GetTextMetricsA", g::g_GetTextMetricsA},
    {"GetSystemPaletteEntries", g::g_GetSystemPaletteEntries},
    {"GetDIBColorTable", g::g_GetDIBColorTable},
    {"SetDIBColorTable", g::g_SetDIBColorTable},
    {nullptr, nullptr},
};
