// Gdi: blits, fills and text. Every raster operation is evaluated from its ROP3 truth table
// (bits 16-23 of the code), bit by bit over the pattern (the DC's brush colour), the source and
// the destination, so SRCCOPY, SRCPAINT, SRCAND, SRCINVERT, PATCOPY, DSTINVERT, BLACKNESS,
// WHITENESS and the rest all take the one path; SRCCOPY between bitmaps of one format above 8
// bits copies rows.
#include "hle/gdi32/Gdi.h"

#include <algorithm>
#include <cstring>

namespace sfcop::hle::gdi32 {

namespace {

std::uint32_t applyRop(std::uint8_t code, std::uint32_t pattern, std::uint32_t source,
                       std::uint32_t destination) {
    std::uint32_t result = 0;
    for (int index = 0; index < 8; ++index) {
        if ((code >> index & 1u) == 0) {
            continue;
        }
        result |= ((index & 4) != 0 ? pattern : ~pattern) & ((index & 2) != 0 ? source : ~source) &
                  ((index & 1) != 0 ? destination : ~destination);
    }
    return result & 0xFFFFFFu;
}

bool usesSource(std::uint8_t code) {
    return ((code >> 2 ^ code) & 0x33u) != 0;
}

bool usesPattern(std::uint8_t code) {
    return ((code >> 4 ^ code) & 0x0Fu) != 0;
}

// `area` clipped to 0..width, 0..height; false if nothing is left.
bool clip(Rect &area, std::int32_t width, std::int32_t height) {
    area.left = std::max(area.left, 0);
    area.top = std::max(area.top, 0);
    area.right = std::min(area.right, width);
    area.bottom = std::min(area.bottom, height);
    return area.left < area.right && area.top < area.bottom;
}

} // namespace

bool Gdi::rop3(std::uint32_t dstDc, const Rect &area, std::uint32_t srcDc, const Rect &from,
               std::uint32_t rop) {
    Dc *d = dc(dstDc);
    if (d == nullptr) {
        return false;
    }
    const auto code = static_cast<std::uint8_t>(rop >> 16);
    Bitmap *target = BitmapOf(d->bitmap);
    Bitmap *source = nullptr;
    if (usesSource(code)) {
        if (dc(srcDc) == nullptr) {
            return false;
        }
        source = SelectedBitmap(srcDc);
        if (source == nullptr) {
            return true; // the screen DC: nothing to read, as nothing is drawn there
        }
    }
    if (target == nullptr) {
        return true; // the screen DC draws nowhere
    }
    const Object *brush = object(d->brush);
    const std::uint32_t pattern =
        brush != nullptr && !brush->hollow ? pixelOf(brush->color) : 0x000000u;
    if (usesPattern(code) && brush != nullptr && brush->hollow) {
        return true;
    }
    const std::int32_t width = area.right - area.left, height = area.bottom - area.top;
    const std::int32_t srcWidth = from.right - from.left, srcHeight = from.bottom - from.top;
    if (width == 0 || height == 0) {
        return true;
    }
    // A negative extent runs back from its origin, which it includes: the destination covers
    // origin + extent + 1 to origin, and the source is read from its origin downwards.
    Rect out = width > 0 ? Rect{area.left, 0, area.right, 0}
                         : Rect{area.right + 1, 0, area.left + 1, 0};
    out.top = height > 0 ? area.top : area.bottom + 1;
    out.bottom = height > 0 ? area.bottom : area.top + 1;
    const Rect whole = out;
    if (!clip(out, target->Width(), target->Height())) {
        return true;
    }
    const bool straight = source != nullptr && width == srcWidth && height == srcHeight &&
                          width > 0 && height > 0;
    if (straight && rop == kSrcCopy && source->BitCount() == target->BitCount() &&
        source->BitCount() > 8 && source != target) {
        Rect in{from.left + (out.left - whole.left), from.top + (out.top - whole.top), 0, 0};
        in.right = in.left + (out.right - out.left);
        in.bottom = in.top + (out.bottom - out.top);
        Rect checked = in;
        if (clip(checked, source->Width(), source->Height()) && checked.left == in.left &&
            checked.top == in.top && checked.right == in.right && checked.bottom == in.bottom) {
            const std::size_t bytes = static_cast<std::size_t>(out.right - out.left) *
                                      source->BitCount() / 8u;
            for (std::int32_t y = 0; y < out.bottom - out.top; ++y) {
                std::memcpy(target->Row(out.top + y) + std::size_t{static_cast<std::uint32_t>(out.left)} *
                                                           target->BitCount() / 8u,
                            source->Row(in.top + y) + std::size_t{static_cast<std::uint32_t>(in.left)} *
                                                          source->BitCount() / 8u,
                            bytes);
            }
            return true;
        }
    }
    for (std::int32_t y = out.top; y < out.bottom; ++y) {
        for (std::int32_t x = out.left; x < out.right; ++x) {
            std::uint32_t s = 0;
            if (source != nullptr) {
                // Which source pixel lands here: nearest neighbour, mirrored by a negative size.
                const std::int32_t dx = width > 0 ? x - area.left : area.left - x;
                const std::int32_t dy = height > 0 ? y - area.top : area.top - y;
                const std::int32_t aw = width > 0 ? width : -width, ah = height > 0 ? height : -height;
                const std::int32_t sw = srcWidth > 0 ? srcWidth : -srcWidth;
                const std::int32_t sh = srcHeight > 0 ? srcHeight : -srcHeight;
                std::int32_t sx = static_cast<std::int32_t>(static_cast<std::int64_t>(dx) * sw / aw);
                std::int32_t sy = static_cast<std::int32_t>(static_cast<std::int64_t>(dy) * sh / ah);
                sx = srcWidth > 0 ? from.left + sx : from.left - sx;
                sy = srcHeight > 0 ? from.top + sy : from.top - sy;
                if (sx < 0 || sy < 0 || sx >= source->Width() || sy >= source->Height()) {
                    continue;
                }
                s = source->Get(sx, sy);
            }
            const std::uint32_t current = target->Get(x, y);
            const std::uint32_t result = applyRop(code, pattern, s, current);
            if (result != current) {
                target->Set(x, y, result);
            }
        }
    }
    return true;
}

bool Gdi::BitBlt(std::uint32_t dst, std::int32_t x, std::int32_t y, std::int32_t width,
                 std::int32_t height, std::uint32_t src, std::int32_t srcX, std::int32_t srcY,
                 std::uint32_t rop) {
    return rop3(dst, Rect{x, y, x + width, y + height}, src,
                Rect{srcX, srcY, srcX + width, srcY + height}, rop);
}

bool Gdi::StretchBlt(std::uint32_t dst, std::int32_t x, std::int32_t y, std::int32_t width,
                     std::int32_t height, std::uint32_t src, std::int32_t srcX, std::int32_t srcY,
                     std::int32_t srcWidth, std::int32_t srcHeight, std::uint32_t rop) {
    return rop3(dst, Rect{x, y, x + width, y + height}, src,
                Rect{srcX, srcY, srcX + srcWidth, srcY + srcHeight}, rop);
}

bool Gdi::PatBlt(std::uint32_t handle, std::int32_t x, std::int32_t y, std::int32_t width,
                 std::int32_t height, std::uint32_t rop) {
    if (usesSource(static_cast<std::uint8_t>(rop >> 16))) {
        return false; // PatBlt takes no source
    }
    return rop3(handle, Rect{x, y, x + width, y + height}, 0, Rect{}, rop);
}

bool Gdi::FillSolid(std::uint32_t handle, const Rect &rect, Colorref color) {
    Dc *d = dc(handle);
    if (d == nullptr) {
        return false;
    }
    Bitmap *target = BitmapOf(d->bitmap);
    Rect area = rect;
    if (target == nullptr || !clip(area, target->Width(), target->Height())) {
        return true;
    }
    const std::uint32_t pixel = pixelOf(color);
    for (std::int32_t y = area.top; y < area.bottom; ++y) {
        for (std::int32_t x = area.left; x < area.right; ++x) {
            target->Set(x, y, pixel);
        }
    }
    return true;
}

bool Gdi::FillRect(std::uint32_t handle, const Rect &rect, std::uint32_t brush) {
    if (dc(handle) == nullptr) {
        return false;
    }
    if (brush != 0 && brush <= 25) {
        return FillSolid(handle, rect, systemColor(brush - 1));
    }
    const Object *o = object(brush);
    if (o == nullptr || o->kind != Kind::kBrush) {
        return false;
    }
    return o->hollow ? true : FillSolid(handle, rect, o->color);
}

void Gdi::drawText(Dc &d, Bitmap &target, std::int32_t x, std::int32_t y, std::string_view text,
                   const Rect *clipRect, const std::int32_t *dx) {
    const Object *font = object(d.font);
    const FontDesc desc = font != nullptr ? font->font : FontDesc{};
    const std::int32_t scale = fontScale(desc.height);
    const TextMetrics metrics = fontMetrics(desc);
    std::int32_t advance = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        advance += dx != nullptr ? dx[i] : kCellWidth * scale;
    }
    const std::uint32_t horizontal = d.align & 6u;
    if (horizontal == kTaCenter) {
        x -= advance / 2;
    } else if (horizontal == kTaRight) {
        x -= advance;
    }
    if ((d.align & kTaBaseline) == kTaBaseline) {
        y -= metrics.ascent;
    } else if ((d.align & kTaBottom) != 0) {
        y -= metrics.height;
    }
    Rect bounds{0, 0, target.Width(), target.Height()};
    if (clipRect != nullptr) {
        bounds.left = std::max(bounds.left, clipRect->left);
        bounds.top = std::max(bounds.top, clipRect->top);
        bounds.right = std::min(bounds.right, clipRect->right);
        bounds.bottom = std::min(bounds.bottom, clipRect->bottom);
    }
    const std::uint32_t ink = pixelOf(d.text), paper = pixelOf(d.background);
    const bool bold = desc.weight >= 600;
    std::int32_t penX = x;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const std::uint8_t *rows = glyphRows(text[i]);
        const std::int32_t cell = dx != nullptr ? dx[i] : kCellWidth * scale;
        for (std::int32_t row = 0; row < kCellHeight * scale; ++row) {
            const std::int32_t py = y + row;
            if (py < bounds.top || py >= bounds.bottom) {
                continue;
            }
            const std::int32_t glyphRow = row / scale;
            const std::uint8_t bits = glyphRow < kGlyphRows ? rows[glyphRow] : 0;
            const bool underline = desc.underline && glyphRow == kGlyphRows;
            for (std::int32_t column = 0; column < cell; ++column) {
                const std::int32_t px = penX + column;
                if (px < bounds.left || px >= bounds.right) {
                    continue;
                }
                const std::int32_t glyphColumn = column / scale;
                bool on = glyphColumn < 5 && (bits >> (4 - glyphColumn) & 1u) != 0;
                if (bold && !on && glyphColumn >= 1 && glyphColumn < 6) {
                    on = (bits >> (5 - glyphColumn) & 1u) != 0;
                }
                if (on || underline) {
                    target.Set(px, py, ink);
                } else if (d.bkMode == kOpaque) {
                    target.Set(px, py, paper);
                }
            }
        }
        penX += cell;
    }
}

bool Gdi::TextOut(std::uint32_t handle, std::int32_t x, std::int32_t y, std::string_view text) {
    return ExtTextOut(handle, x, y, 0, nullptr, text, nullptr);
}

bool Gdi::ExtTextOut(std::uint32_t handle, std::int32_t x, std::int32_t y, std::uint32_t options,
                     const Rect *rect, std::string_view text, const std::int32_t *dx) {
    Dc *d = dc(handle);
    if (d == nullptr) {
        return false;
    }
    Bitmap *target = BitmapOf(d->bitmap);
    if (target == nullptr) {
        return true;
    }
    if (rect != nullptr && (options & kEtoOpaque) != 0) {
        Rect area = *rect;
        if (clip(area, target->Width(), target->Height())) {
            for (std::int32_t py = area.top; py < area.bottom; ++py) {
                for (std::int32_t px = area.left; px < area.right; ++px) {
                    target->Set(px, py, pixelOf(d->background));
                }
            }
        }
    }
    drawText(*d, *target, x, y, text,
             rect != nullptr && (options & kEtoClipped) != 0 ? rect : nullptr, dx);
    return true;
}

} // namespace sfcop::hle::gdi32
