// GDI on host bitmaps (DESIGN §5.6, §6.1): device contexts, DIB sections, stock objects, blits
// with the raster operations the game uses, and text in the module's own font (Font.h).
//
// A device context draws into the bitmap selected into it. A memory DC (CreateCompatibleDC)
// starts with a 1x1 bitmap of its own, as GDI's does; a DC made with DcOnBitmap() draws into a
// bitmap that belongs to someone else (a window's surface, from the USER32 module) and never
// gives it up; the screen DC (GetDC of no window) has no bitmap and answers only GetDeviceCaps.
// The screen is the game's display (ScreenInfo): its size and depth are what GetDeviceCaps
// reports for every DC, as GDI does for a memory DC.
//
// Handles come from a HandleSource: in the guest, entries of win32hle's handle table of this
// module's types (src/hle/README.md, "Handle types"); in tests, counters. A handle this module did
// not make is "foreign": every call returns its failure value for it, and the guest layer hands
// the call on (Gdi32Guest.cpp).
//
// Not thread-safe: the guest layer calls it under the machine lock.
#pragma once

#include "hle/gdi32/Bitmap.h"
#include "hle/gdi32/Font.h"
#include "hle/gdi32/Gdi32Defs.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string_view>
#include <vector>

namespace sfcop::hle::gdi32 {

struct Rect {
    std::int32_t left = 0, top = 0, right = 0, bottom = 0;
};

struct Size {
    std::int32_t cx = 0, cy = 0;
};

struct ScreenInfo {
    std::int32_t width = 640;
    std::int32_t height = 480;
    std::int32_t bitsPerPixel = 32;
    std::int32_t refreshHz = 60;
};

enum class HandleKind : std::uint8_t { kDc, kObject };

struct HandleSource {
    std::function<std::uint32_t(HandleKind)> allocate; // 0: none left
    std::function<void(std::uint32_t)> release;
};

// A HandleSource for tests and for a host without win32hle: DCs from 0x1000, objects from
// 0x2000, four apart.
HandleSource countingHandles();

// One RGBQUAD of a colour table or a system palette entry, as the game's structures hold them.
struct Quad {
    std::uint8_t blue = 0, green = 0, red = 0, reserved = 0;
};

// The system colours FillRect accepts as COLOR_* + 1 in place of a brush: COLOR_SCROLLBAR (0) to
// COLOR_INFOBK (24), the classic grey scheme.
Colorref systemColor(std::uint32_t index);

class Gdi {
public:
    Gdi(PixelMemory &memory, HandleSource handles, std::function<ScreenInfo()> screen);
    ~Gdi();
    Gdi(const Gdi &) = delete;
    Gdi &operator=(const Gdi &) = delete;

    bool OwnsDc(std::uint32_t dc) const;
    bool OwnsObject(std::uint32_t object) const;

    // ---- device contexts ----
    // A memory DC compatible with `dc` (0 or any DC: there is one screen). 0 on failure.
    std::uint32_t CreateCompatibleDC(std::uint32_t dc);
    // A DC drawing into the bitmap `bitmap` (an object handle), which the DC never deletes; with
    // 0, the screen DC. 0 on failure.
    std::uint32_t DcOnBitmap(std::uint32_t bitmap);
    bool DeleteDC(std::uint32_t dc);
    std::uint32_t GetDeviceCaps(std::uint32_t dc, std::uint32_t index) const;

    // ---- objects ----
    // A DIB section; `*bits` gets its pixels. 0 on failure (an unsupported format, a foreign DC).
    std::uint32_t CreateDIBSection(std::uint32_t dc, const BitmapFormat &format,
                                   std::uint8_t **bits);
    // A host-owned bitmap of 32 bits, top-down (a window's surface).
    std::uint32_t CreateSurfaceBitmap(std::int32_t width, std::int32_t height);
    std::uint32_t CreateFont(const FontDesc &font);
    std::uint32_t GetStockObject(std::uint32_t index);
    // The previous object of that kind; kHgdiError for a bitmap selected elsewhere, 0 for a bad
    // handle.
    std::uint32_t SelectObject(std::uint32_t dc, std::uint32_t object);
    // False for a bad handle; a stock object is never deleted and answers true; a bitmap or font
    // selected into a DC is deleted when the DC lets it go (GDI leaves that undefined).
    bool DeleteObject(std::uint32_t object);

    // ---- drawing ----
    bool BitBlt(std::uint32_t dst, std::int32_t x, std::int32_t y, std::int32_t width,
                std::int32_t height, std::uint32_t src, std::int32_t srcX, std::int32_t srcY,
                std::uint32_t rop);
    bool StretchBlt(std::uint32_t dst, std::int32_t x, std::int32_t y, std::int32_t width,
                    std::int32_t height, std::uint32_t src, std::int32_t srcX, std::int32_t srcY,
                    std::int32_t srcWidth, std::int32_t srcHeight, std::uint32_t rop);
    bool PatBlt(std::uint32_t dc, std::int32_t x, std::int32_t y, std::int32_t width,
                std::int32_t height, std::uint32_t rop);
    // USER32's FillRect: `brush` is a brush handle or a system colour index + 1.
    bool FillRect(std::uint32_t dc, const Rect &rect, std::uint32_t brush);
    // A rectangle of one colour, for USER32's own drawing (dialogs).
    bool FillSolid(std::uint32_t dc, const Rect &rect, Colorref color);

    // ---- text ----
    Colorref SetTextColor(std::uint32_t dc, Colorref color);
    Colorref SetBkColor(std::uint32_t dc, Colorref color);
    std::uint32_t SetBkMode(std::uint32_t dc, std::uint32_t mode); // 0 on failure
    std::uint32_t SetTextAlign(std::uint32_t dc, std::uint32_t align); // GDI_ERROR on failure
    bool TextOut(std::uint32_t dc, std::int32_t x, std::int32_t y, std::string_view text);
    bool ExtTextOut(std::uint32_t dc, std::int32_t x, std::int32_t y, std::uint32_t options,
                    const Rect *rect, std::string_view text, const std::int32_t *dx);
    bool GetTextExtent(std::uint32_t dc, std::string_view text, Size &size) const;
    bool GetTextMetrics(std::uint32_t dc, TextMetrics &metrics) const;

    // ---- palettes ----
    // Entries of the system palette (the twenty static colours, black between); the count copied.
    std::uint32_t GetSystemPaletteEntries(std::uint32_t dc, std::uint32_t start, std::uint32_t count,
                                          Quad *out) const;
    // The colour table of the DIB section selected into `dc`; the count copied, 0 for none.
    std::uint32_t GetDIBColorTable(std::uint32_t dc, std::uint32_t start, std::uint32_t count,
                                   Quad *out) const;
    std::uint32_t SetDIBColorTable(std::uint32_t dc, std::uint32_t start, std::uint32_t count,
                                   const Quad *in);

    // The bitmap of an object handle, or of the one selected into a DC; nullptr if none.
    Bitmap *BitmapOf(std::uint32_t object) const;
    Bitmap *SelectedBitmap(std::uint32_t dc) const;

private:
    enum class Kind : std::uint8_t { kBitmap, kBrush, kPen, kFont, kPalette };
    struct Object {
        Kind kind = Kind::kBrush;
        std::unique_ptr<Bitmap> bitmap;
        Colorref color = 0;   // brush, pen
        bool hollow = false;  // NULL_BRUSH, NULL_PEN
        FontDesc font;
        bool stock = false;
        std::uint32_t selectedIn = 0; // a bitmap's DC
        bool deletePending = false;
    };
    struct Dc {
        std::uint32_t bitmap = 0; // object handle, or 0 for the screen DC
        std::uint32_t ownBitmap = 0; // the 1x1 a memory DC starts with
        bool borrowed = false;       // DcOnBitmap: the bitmap is not this DC's
        std::uint32_t brush = 0, pen = 0, font = 0;
        Colorref text = 0, background = 0xFFFFFFu;
        std::uint32_t bkMode = kOpaque, align = 0;
    };

    Object *object(std::uint32_t handle) const;
    Dc *dc(std::uint32_t handle) const;
    std::uint32_t newObject(Object made);
    void releaseObject(std::uint32_t handle);
    void drawText(Dc &d, Bitmap &target, std::int32_t x, std::int32_t y, std::string_view text,
                  const Rect *clip, const std::int32_t *dx);
    bool rop3(std::uint32_t dst, const Rect &area, std::uint32_t src, const Rect &from,
              std::uint32_t rop);

    PixelMemory &memory_;
    HandleSource handles_;
    std::function<ScreenInfo()> screen_;
    std::map<std::uint32_t, Object> objects_;
    std::map<std::uint32_t, Dc> dcs_;
    std::array<std::uint32_t, kStockObjectCount> stock_{};
};

} // namespace sfcop::hle::gdi32
