// Gdi: handles, device contexts, objects and palettes. The drawing is in GdiDraw.cpp.
#include "hle/gdi32/Gdi.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace sfcop::hle::gdi32 {

namespace {

// The twenty static colours of the default system palette, first ten and last ten.
constexpr std::uint32_t kStaticColors[20] = {
    0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xC0C0C0, 0xC0DCC0,
    0xA6CAF0, 0xFFFBF0, 0xA0A0A4, 0x808080, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF,
    0x00FFFF, 0xFFFFFF,
}; // 0x00RRGGBB

// COLOR_SCROLLBAR to COLOR_INFOBK, as COLORREFs.
constexpr Colorref kSystemColors[25] = {
    0xC0C0C0, 0x808000, 0x800000, 0x808080, 0xC0C0C0, 0xFFFFFF, 0x000000, 0x000000,
    0x000000, 0xFFFFFF, 0xC0C0C0, 0xC0C0C0, 0x808080, 0x800000, 0xFFFFFF, 0xC0C0C0,
    0x808080, 0x808080, 0x000000, 0xC0C0C0, 0xFFFFFF, 0x000000, 0xDFDFDF, 0x000000, 0xE1FFFF,
};

} // namespace

HandleSource countingHandles() {
    auto next = std::make_shared<std::array<std::uint32_t, 2>>(
        std::array<std::uint32_t, 2>{0x1000u, 0x2000u});
    return HandleSource{
        [next](HandleKind kind) {
            std::uint32_t &counter = (*next)[kind == HandleKind::kDc ? 0 : 1];
            counter += 4;
            return counter;
        },
        [](std::uint32_t) {}};
}

Colorref systemColor(std::uint32_t index) {
    return index < 25 ? kSystemColors[index] : 0;
}

Gdi::Gdi(PixelMemory &memory, HandleSource handles, std::function<ScreenInfo()> screen)
    : memory_(memory), handles_(std::move(handles)), screen_(std::move(screen)) {}

Gdi::~Gdi() = default;

bool Gdi::OwnsDc(std::uint32_t handle) const {
    return dcs_.count(handle) != 0;
}

bool Gdi::OwnsObject(std::uint32_t handle) const {
    return objects_.count(handle) != 0;
}

Gdi::Object *Gdi::object(std::uint32_t handle) const {
    const auto found = objects_.find(handle);
    return found == objects_.end() ? nullptr : const_cast<Object *>(&found->second);
}

Gdi::Dc *Gdi::dc(std::uint32_t handle) const {
    const auto found = dcs_.find(handle);
    return found == dcs_.end() ? nullptr : const_cast<Dc *>(&found->second);
}

std::uint32_t Gdi::newObject(Object made) {
    const std::uint32_t handle = handles_.allocate(HandleKind::kObject);
    if (handle != 0) {
        objects_.emplace(handle, std::move(made));
    }
    return handle;
}

void Gdi::releaseObject(std::uint32_t handle) {
    objects_.erase(handle);
    handles_.release(handle);
}

// ---- device contexts ----

std::uint32_t Gdi::CreateCompatibleDC(std::uint32_t source) {
    if (source != 0 && !OwnsDc(source)) {
        return 0;
    }
    BitmapFormat format;
    format.width = 1;
    format.height = 1;
    format.bitCount = 32;
    Object one;
    one.kind = Kind::kBitmap;
    one.bitmap = Bitmap::Make(format, memory_);
    if (one.bitmap == nullptr) {
        return 0;
    }
    const std::uint32_t bitmap = newObject(std::move(one));
    const std::uint32_t handle = bitmap == 0 ? 0 : handles_.allocate(HandleKind::kDc);
    if (handle == 0) {
        if (bitmap != 0) {
            releaseObject(bitmap);
        }
        return 0;
    }
    Dc made;
    made.bitmap = made.ownBitmap = bitmap;
    made.brush = GetStockObject(kWhiteBrush);
    made.pen = GetStockObject(kBlackPen);
    made.font = GetStockObject(kSystemFont);
    objects_[bitmap].selectedIn = handle;
    dcs_.emplace(handle, made);
    return handle;
}

std::uint32_t Gdi::DcOnBitmap(std::uint32_t bitmap) {
    if (bitmap != 0 && BitmapOf(bitmap) == nullptr) {
        return 0;
    }
    const std::uint32_t handle = handles_.allocate(HandleKind::kDc);
    if (handle == 0) {
        return 0;
    }
    Dc made;
    made.bitmap = bitmap;
    made.borrowed = true;
    made.brush = GetStockObject(kWhiteBrush);
    made.pen = GetStockObject(kBlackPen);
    made.font = GetStockObject(kSystemFont);
    dcs_.emplace(handle, made);
    return handle;
}

bool Gdi::DeleteDC(std::uint32_t handle) {
    Dc *d = dc(handle);
    if (d == nullptr) {
        return false;
    }
    if (!d->borrowed && d->bitmap != 0 && d->bitmap != d->ownBitmap) {
        if (Object *selected = object(d->bitmap)) {
            selected->selectedIn = 0;
            if (selected->deletePending) {
                releaseObject(d->bitmap);
            }
        }
    }
    // The 1x1 bitmap the DC started with goes with it.
    if (d->ownBitmap != 0 && object(d->ownBitmap) != nullptr) {
        releaseObject(d->ownBitmap);
    }
    dcs_.erase(handle);
    handles_.release(handle);
    return true;
}

std::uint32_t Gdi::GetDeviceCaps(std::uint32_t handle, std::uint32_t index) const {
    if (dc(handle) == nullptr) {
        return 0;
    }
    const ScreenInfo screen = screen_();
    switch (index) {
    case kDriverVersion:
        return 0x400;
    case kTechnology:
        return kDtRasDisplay;
    case kHorzSize: // millimetres at 96 dots an inch
        return static_cast<std::uint32_t>(screen.width * 254 / 960);
    case kVertSize:
        return static_cast<std::uint32_t>(screen.height * 254 / 960);
    case kHorzRes:
        return static_cast<std::uint32_t>(screen.width);
    case kVertRes:
        return static_cast<std::uint32_t>(screen.height);
    case kBitsPixel:
        return static_cast<std::uint32_t>(screen.bitsPerPixel);
    case kPlanes:
        return 1;
    case kNumColors:
        return screen.bitsPerPixel <= 8 ? 20u : 0xFFFFFFFFu;
    case kRasterCaps:
        return kRasterCapsValue | (screen.bitsPerPixel <= 8 ? 0x0100u : 0u); // RC_PALETTE
    case kLogPixelsX:
    case kLogPixelsY:
        return 96;
    case kSizePalette:
        return screen.bitsPerPixel <= 8 ? 256u : 0u;
    case kNumReserved:
        return screen.bitsPerPixel <= 8 ? 20u : 0u;
    case kColorRes:
        return 24;
    case kVRefresh:
        return static_cast<std::uint32_t>(screen.refreshHz);
    default:
        return 0;
    }
}

// ---- objects ----

std::uint32_t Gdi::CreateDIBSection(std::uint32_t handle, const BitmapFormat &format,
                                    std::uint8_t **bits) {
    if (handle != 0 && !OwnsDc(handle)) {
        return 0;
    }
    Object made;
    made.kind = Kind::kBitmap;
    made.bitmap = Bitmap::Make(format, memory_);
    if (made.bitmap == nullptr) {
        return 0;
    }
    std::uint8_t *pixels = made.bitmap->Bits();
    const std::uint32_t object = newObject(std::move(made));
    if (bits != nullptr) {
        *bits = object == 0 ? nullptr : pixels;
    }
    return object;
}

std::uint32_t Gdi::CreateSurfaceBitmap(std::int32_t width, std::int32_t height) {
    BitmapFormat format;
    format.width = width;
    format.height = -height;
    format.bitCount = 32;
    return CreateDIBSection(0, format, nullptr);
}

std::uint32_t Gdi::CreateFont(const FontDesc &font) {
    Object made;
    made.kind = Kind::kFont;
    made.font = font;
    return newObject(std::move(made));
}

std::uint32_t Gdi::GetStockObject(std::uint32_t index) {
    if (index >= kStockObjectCount || index == 9) {
        return 0;
    }
    if (stock_[index] != 0) {
        return stock_[index];
    }
    Object made;
    made.stock = true;
    switch (index) {
    case kWhiteBrush:
    case kLtGrayBrush:
    case kGrayBrush:
    case kDkGrayBrush:
    case kBlackBrush:
    case kNullBrush:
    case kDcBrush: {
        constexpr Colorref kBrushColors[] = {0xFFFFFF, 0xC0C0C0, 0x808080, 0x404040, 0x000000};
        made.kind = Kind::kBrush;
        made.hollow = index == kNullBrush;
        made.color = index <= kBlackBrush ? kBrushColors[index] : 0xFFFFFF;
        break;
    }
    case kWhitePen:
    case kBlackPen:
    case kNullPen:
    case kDcPen:
        made.kind = Kind::kPen;
        made.hollow = index == kNullPen;
        made.color = index == kWhitePen ? 0xFFFFFFu : 0u;
        break;
    case kDefaultPalette:
        made.kind = Kind::kPalette;
        break;
    default:
        made.kind = Kind::kFont;
        made.font.height = kCellHeight;
        made.font.face = "System";
        break;
    }
    stock_[index] = newObject(std::move(made));
    return stock_[index];
}

std::uint32_t Gdi::SelectObject(std::uint32_t handle, std::uint32_t selected) {
    Dc *d = dc(handle);
    Object *o = object(selected);
    if (d == nullptr || o == nullptr) {
        return 0;
    }
    std::uint32_t previous = 0;
    switch (o->kind) {
    case Kind::kBitmap: {
        if (o->selectedIn != 0 && o->selectedIn != handle) {
            return kHgdiError;
        }
        previous = d->bitmap;
        if (Object *old = object(previous); old != nullptr && !d->borrowed && previous != selected) {
            old->selectedIn = 0;
            if (old->deletePending) {
                releaseObject(previous);
            }
        }
        d->bitmap = selected;
        d->borrowed = false;
        o->selectedIn = handle;
        break;
    }
    case Kind::kBrush:
        previous = d->brush;
        d->brush = selected;
        break;
    case Kind::kPen:
        previous = d->pen;
        d->pen = selected;
        break;
    case Kind::kFont:
        previous = d->font;
        d->font = selected;
        break;
    case Kind::kPalette:
        previous = GetStockObject(kDefaultPalette);
        break;
    }
    return previous;
}

bool Gdi::DeleteObject(std::uint32_t handle) {
    Object *o = object(handle);
    if (o == nullptr) {
        return false;
    }
    if (o->stock) {
        return true;
    }
    if (o->kind == Kind::kBitmap && o->selectedIn != 0) {
        o->deletePending = true;
        return true;
    }
    for (auto &entry : dcs_) {
        Dc &d = entry.second;
        if (d.brush == handle) {
            d.brush = GetStockObject(kWhiteBrush);
        }
        if (d.pen == handle) {
            d.pen = GetStockObject(kBlackPen);
        }
        if (d.font == handle) {
            d.font = GetStockObject(kSystemFont);
        }
        if (d.borrowed && d.bitmap == handle) {
            d.bitmap = 0;
        }
    }
    releaseObject(handle);
    return true;
}

Bitmap *Gdi::BitmapOf(std::uint32_t handle) const {
    const Object *o = object(handle);
    return o != nullptr && o->kind == Kind::kBitmap ? o->bitmap.get() : nullptr;
}

Bitmap *Gdi::SelectedBitmap(std::uint32_t handle) const {
    const Dc *d = dc(handle);
    return d == nullptr ? nullptr : BitmapOf(d->bitmap);
}

// ---- text state ----

Colorref Gdi::SetTextColor(std::uint32_t handle, Colorref color) {
    Dc *d = dc(handle);
    if (d == nullptr) {
        return kClrInvalid;
    }
    return std::exchange(d->text, color & 0xFFFFFFu);
}

Colorref Gdi::SetBkColor(std::uint32_t handle, Colorref color) {
    Dc *d = dc(handle);
    if (d == nullptr) {
        return kClrInvalid;
    }
    return std::exchange(d->background, color & 0xFFFFFFu);
}

std::uint32_t Gdi::SetBkMode(std::uint32_t handle, std::uint32_t mode) {
    Dc *d = dc(handle);
    if (d == nullptr || (mode != kTransparent && mode != kOpaque)) {
        return 0;
    }
    return std::exchange(d->bkMode, mode);
}

std::uint32_t Gdi::SetTextAlign(std::uint32_t handle, std::uint32_t align) {
    Dc *d = dc(handle);
    if (d == nullptr) {
        return 0xFFFFFFFFu;
    }
    return std::exchange(d->align, align);
}

bool Gdi::GetTextMetrics(std::uint32_t handle, TextMetrics &metrics) const {
    const Dc *d = dc(handle);
    const Object *font = d == nullptr ? nullptr : object(d->font);
    if (font == nullptr) {
        return false;
    }
    metrics = fontMetrics(font->font);
    return true;
}

bool Gdi::GetTextExtent(std::uint32_t handle, std::string_view text, Size &size) const {
    const Dc *d = dc(handle);
    const Object *font = d == nullptr ? nullptr : object(d->font);
    if (font == nullptr) {
        return false;
    }
    const std::int32_t scale = fontScale(font->font.height);
    size.cx = static_cast<std::int32_t>(text.size()) * kCellWidth * scale;
    size.cy = kCellHeight * scale;
    return true;
}

// ---- palettes ----

std::uint32_t Gdi::GetSystemPaletteEntries(std::uint32_t handle, std::uint32_t start,
                                           std::uint32_t count, Quad *out) const {
    if (dc(handle) == nullptr) {
        return 0;
    }
    if (out == nullptr) {
        return 256;
    }
    std::uint32_t copied = 0;
    for (std::uint32_t i = start; i < 256 && copied < count; ++i, ++copied) {
        std::uint32_t pixel = 0;
        if (i < 10) {
            pixel = kStaticColors[i];
        } else if (i >= 246) {
            pixel = kStaticColors[i - 236];
        }
        out[copied] = Quad{static_cast<std::uint8_t>(pixel), static_cast<std::uint8_t>(pixel >> 8),
                           static_cast<std::uint8_t>(pixel >> 16), 0};
    }
    return copied;
}

std::uint32_t Gdi::GetDIBColorTable(std::uint32_t handle, std::uint32_t start, std::uint32_t count,
                                    Quad *out) const {
    const Bitmap *bitmap = SelectedBitmap(handle);
    if (bitmap == nullptr || bitmap->BitCount() > 8 || out == nullptr) {
        return 0;
    }
    std::uint32_t copied = 0;
    for (std::uint32_t i = start; i < bitmap->ColorCount() && copied < count; ++i, ++copied) {
        const std::uint32_t pixel = bitmap->Color(i);
        out[copied] = Quad{static_cast<std::uint8_t>(pixel), static_cast<std::uint8_t>(pixel >> 8),
                           static_cast<std::uint8_t>(pixel >> 16), 0};
    }
    return copied;
}

std::uint32_t Gdi::SetDIBColorTable(std::uint32_t handle, std::uint32_t start, std::uint32_t count,
                                    const Quad *in) {
    Bitmap *bitmap = SelectedBitmap(handle);
    if (bitmap == nullptr || bitmap->BitCount() > 8 || in == nullptr) {
        return 0;
    }
    std::uint32_t copied = 0;
    for (std::uint32_t i = start; i < bitmap->ColorCount() && copied < count; ++i, ++copied) {
        bitmap->SetColor(i, static_cast<std::uint32_t>(in[copied].red) << 16 |
                                static_cast<std::uint32_t>(in[copied].green) << 8 |
                                in[copied].blue);
    }
    return copied;
}

} // namespace sfcop::hle::gdi32
