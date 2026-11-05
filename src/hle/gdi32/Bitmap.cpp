#include "hle/gdi32/Bitmap.h"

#include <cstdlib>
#include <cstring>

namespace sfcop::hle::gdi32 {

namespace {

constexpr std::int32_t kMaxDimension = 16384;

int lowestBit(std::uint32_t mask) {
    int shift = 0;
    while (shift < 32 && (mask >> shift & 1u) == 0) {
        ++shift;
    }
    return shift;
}

std::uint32_t maskMax(std::uint32_t mask) {
    return mask == 0 ? 0 : mask >> lowestBit(mask);
}

// A channel of `pixel` under `mask`, scaled to 0-255.
std::uint32_t channel(std::uint32_t pixel, std::uint32_t mask) {
    const std::uint32_t max = maskMax(mask);
    return max == 0 ? 0 : ((pixel & mask) >> lowestBit(mask)) * 255u / max;
}

// An 8-bit channel value placed under `mask`, rounded to the mask's precision.
std::uint32_t place(std::uint32_t value, std::uint32_t mask) {
    const std::uint32_t max = maskMax(mask);
    return max == 0 ? 0 : ((value * max + 127u) / 255u) << lowestBit(mask) & mask;
}

} // namespace

std::uint8_t *HeapPixelMemory::Allocate(std::size_t bytes) {
    return static_cast<std::uint8_t *>(std::calloc(bytes == 0 ? 1 : bytes, 1));
}

void HeapPixelMemory::Free(std::uint8_t *block) {
    std::free(block);
}

std::unique_ptr<Bitmap> Bitmap::Make(const BitmapFormat &format, PixelMemory &memory) {
    const std::int32_t height = format.height < 0 ? -format.height : format.height;
    if (format.width <= 0 || height <= 0 || format.width > kMaxDimension || height > kMaxDimension) {
        return nullptr;
    }
    switch (format.bitCount) {
    case 8:
    case 16:
    case 24:
    case 32:
        break;
    default:
        return nullptr;
    }
    if (format.compression != 0 && format.compression != 3) {
        return nullptr;
    }
    const std::uint32_t pitch =
        (static_cast<std::uint32_t>(format.width) * format.bitCount + 31u) / 32u * 4u;
    std::uint8_t *bits = memory.Allocate(std::size_t{pitch} * static_cast<std::size_t>(height));
    if (bits == nullptr) {
        return nullptr;
    }
    return std::unique_ptr<Bitmap>(new Bitmap(format, bits, memory));
}

Bitmap::Bitmap(const BitmapFormat &format, std::uint8_t *bits, PixelMemory &memory)
    : width_(format.width), height_(format.height < 0 ? -format.height : format.height),
      bitCount_(format.bitCount),
      pitch_((static_cast<std::uint32_t>(format.width) * format.bitCount + 31u) / 32u * 4u),
      topDown_(format.height < 0), redMask_(format.redMask), greenMask_(format.greenMask),
      blueMask_(format.blueMask), colorCount_(format.colorCount), colors_(format.colors),
      bits_(bits), memory_(memory) {
    const bool masked = format.compression == 3 && (redMask_ | greenMask_ | blueMask_) != 0;
    if (!masked && bitCount_ == 16) {
        redMask_ = 0x7C00u, greenMask_ = 0x03E0u, blueMask_ = 0x001Fu; // BI_RGB at 16 bits is 5-5-5
    } else if (!masked) {
        redMask_ = 0xFF0000u, greenMask_ = 0x00FF00u, blueMask_ = 0x0000FFu;
    }
    if (bitCount_ == 8 && colorCount_ == 0) {
        colorCount_ = 256;
    }
}

Bitmap::~Bitmap() {
    memory_.Free(bits_);
}

std::uint8_t *Bitmap::Row(std::int32_t y) const {
    const std::int32_t stored = topDown_ ? y : height_ - 1 - y;
    return bits_ + std::size_t{pitch_} * static_cast<std::size_t>(stored);
}

std::uint32_t Bitmap::Get(std::int32_t x, std::int32_t y) const {
    const std::uint8_t *row = Row(y);
    switch (bitCount_) {
    case 8:
        return colors_[row[x]];
    case 16: {
        const std::uint32_t pixel = row[2 * x] | static_cast<std::uint32_t>(row[2 * x + 1]) << 8;
        return channel(pixel, redMask_) << 16 | channel(pixel, greenMask_) << 8 |
               channel(pixel, blueMask_);
    }
    case 24:
        return static_cast<std::uint32_t>(row[3 * x + 2]) << 16 |
               static_cast<std::uint32_t>(row[3 * x + 1]) << 8 | row[3 * x];
    default: {
        std::uint32_t pixel;
        std::memcpy(&pixel, row + 4 * x, 4);
        return channel(pixel, redMask_) << 16 | channel(pixel, greenMask_) << 8 |
               channel(pixel, blueMask_);
    }
    }
}

void Bitmap::Set(std::int32_t x, std::int32_t y, std::uint32_t rgb) {
    std::uint8_t *row = Row(y);
    const std::uint32_t red = rgb >> 16 & 0xFFu, green = rgb >> 8 & 0xFFu, blue = rgb & 0xFFu;
    switch (bitCount_) {
    case 8:
        row[x] = nearestIndex(rgb);
        return;
    case 16: {
        const std::uint32_t pixel =
            place(red, redMask_) | place(green, greenMask_) | place(blue, blueMask_);
        row[2 * x] = static_cast<std::uint8_t>(pixel);
        row[2 * x + 1] = static_cast<std::uint8_t>(pixel >> 8);
        return;
    }
    case 24:
        row[3 * x] = static_cast<std::uint8_t>(blue);
        row[3 * x + 1] = static_cast<std::uint8_t>(green);
        row[3 * x + 2] = static_cast<std::uint8_t>(red);
        return;
    default: {
        const std::uint32_t pixel =
            place(red, redMask_) | place(green, greenMask_) | place(blue, blueMask_);
        std::memcpy(row + 4 * x, &pixel, 4);
        return;
    }
    }
}

std::uint8_t Bitmap::nearestIndex(std::uint32_t rgb) const {
    std::uint32_t best = 0, bestDistance = 0xFFFFFFFFu;
    const std::uint32_t count = colorCount_ == 0 ? 256 : colorCount_;
    for (std::uint32_t i = 0; i < count; ++i) {
        const int dr = static_cast<int>(colors_[i] >> 16 & 0xFFu) - static_cast<int>(rgb >> 16 & 0xFFu);
        const int dg = static_cast<int>(colors_[i] >> 8 & 0xFFu) - static_cast<int>(rgb >> 8 & 0xFFu);
        const int db = static_cast<int>(colors_[i] & 0xFFu) - static_cast<int>(rgb & 0xFFu);
        const auto distance = static_cast<std::uint32_t>(dr * dr + dg * dg + db * db);
        if (distance < bestDistance) {
            best = i, bestDistance = distance;
            if (distance == 0) {
                break;
            }
        }
    }
    return static_cast<std::uint8_t>(best);
}

} // namespace sfcop::hle::gdi32
