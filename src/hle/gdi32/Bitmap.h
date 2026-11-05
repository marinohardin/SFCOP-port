// A GDI bitmap: rows of pixels in one of the DIB formats the game uses (8 bits through a colour
// table, 16 bits as 5-5-5 or by masks, 24 and 32 bits), top-down or bottom-up. The pixels live
// where a PixelMemory puts them: a DIB section's must be addressable by the game, which writes
// them directly, so the guest layer allocates them in guest memory; tests use the host heap.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace sfcop::hle::gdi32 {

class PixelMemory {
public:
    virtual ~PixelMemory() = default;

    // A zero-filled block of `bytes`, or nullptr if memory is exhausted.
    virtual std::uint8_t *Allocate(std::size_t bytes) = 0;

    // Returns a block from Allocate(); nullptr does nothing.
    virtual void Free(std::uint8_t *block) = 0;
};

// PixelMemory on the host heap.
class HeapPixelMemory final : public PixelMemory {
public:
    std::uint8_t *Allocate(std::size_t bytes) override;
    void Free(std::uint8_t *block) override;
};

// What a BITMAPINFO describes, read by the caller from the game's structure.
struct BitmapFormat {
    std::int32_t width = 0;
    std::int32_t height = 0; // negative: top-down
    std::uint16_t bitCount = 32;
    std::uint32_t compression = 0; // BI_RGB or BI_BITFIELDS
    std::uint32_t redMask = 0;     // BI_BITFIELDS only
    std::uint32_t greenMask = 0;
    std::uint32_t blueMask = 0;
    std::uint32_t colorCount = 0;          // entries in `colors` (8 bits and fewer)
    std::array<std::uint32_t, 256> colors{}; // 0x00RRGGBB
};

class Bitmap {
public:
    // A bitmap of the format, its pixels from `memory`; nullptr for a format this module does not
    // draw (a bit count other than 8, 16, 24 or 32, a zero or oversize dimension, a compression
    // other than BI_RGB and BI_BITFIELDS) or when memory is exhausted.
    static std::unique_ptr<Bitmap> Make(const BitmapFormat &format, PixelMemory &memory);
    ~Bitmap();
    Bitmap(const Bitmap &) = delete;
    Bitmap &operator=(const Bitmap &) = delete;

    std::int32_t Width() const { return width_; }
    std::int32_t Height() const { return height_; }
    std::uint16_t BitCount() const { return bitCount_; }
    std::uint32_t Pitch() const { return pitch_; }
    bool TopDown() const { return topDown_; }
    std::uint8_t *Bits() const { return bits_; }
    std::size_t Bytes() const { return std::size_t{pitch_} * static_cast<std::size_t>(height_); }

    // The first byte of row `y` counted from the top.
    std::uint8_t *Row(std::int32_t y) const;

    // A pixel as 0x00RRGGBB, and setting one from that form (8 bits: the nearest table entry).
    // Coordinates must lie inside the bitmap.
    std::uint32_t Get(std::int32_t x, std::int32_t y) const;
    void Set(std::int32_t x, std::int32_t y, std::uint32_t rgb);

    // The colour table of a bitmap of 8 bits or fewer.
    std::uint32_t ColorCount() const { return colorCount_; }
    std::uint32_t Color(std::uint32_t index) const { return colors_[index & 0xFFu]; }
    void SetColor(std::uint32_t index, std::uint32_t rgb) { colors_[index & 0xFFu] = rgb; }

private:
    Bitmap(const BitmapFormat &format, std::uint8_t *bits, PixelMemory &memory);
    std::uint8_t nearestIndex(std::uint32_t rgb) const;

    std::int32_t width_;
    std::int32_t height_;
    std::uint16_t bitCount_;
    std::uint32_t pitch_;
    bool topDown_;
    std::uint32_t redMask_, greenMask_, blueMask_;
    std::uint32_t colorCount_;
    std::array<std::uint32_t, 256> colors_;
    std::uint8_t *bits_;
    PixelMemory &memory_;
};

} // namespace sfcop::hle::gdi32
