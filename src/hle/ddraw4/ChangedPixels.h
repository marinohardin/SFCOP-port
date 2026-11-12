// Which pixels of a surface the game changed through Lock or GetDC, for a Direct3D target with a
// render scale (Surface.h). Such a target's host copy is a read-back, each pixel the average of a
// k by k block, so uploading a locked area whole would replace the finer image with its average.
// Surface keeps a copy of the area at Lock, marks here the pixels that differ at Unlock, and at
// the next flush uploads only those: Patch() packs the marked pixels with every other pixel set
// to a key no marked pixel has, for a keyed blit that leaves the rest of the target untouched.
#pragma once

#include "hle/ddraw4/Ddraw4Defs.h"

#include <cstdint>
#include <vector>

namespace sfcop::hle::ddraw4 {

class ChangedPixels {
public:
    // Forgets every mark and sizes the marks to a surface of width by height pixels.
    void Reset(std::uint32_t width, std::uint32_t height);
    // Whether Reset() last gave this size.
    bool SizedFor(std::uint32_t width, std::uint32_t height) const {
        return width_ == width && height_ == height && !marks_.empty();
    }
    // Forgets every mark, keeping the size.
    void Clear();

    // Marks each pixel of `area` whose `pixelBytes` bytes differ between `before` and `after`.
    // Both point at the area's first pixel, their rows `beforePitch` and `afterPitch` bytes
    // apart. `area` lies inside the size Reset() gave.
    void Compare(const RectL &area, const std::uint8_t *before, std::uint32_t beforePitch,
                 const std::uint8_t *after, std::uint32_t afterPitch, std::uint32_t pixelBytes);

    bool Empty() const { return count_ == 0; }
    // The smallest rectangle holding every mark (all zero when empty).
    const RectL &Area() const { return area_; }
    // Whether every pixel of Area() is marked: then the area uploads as it is, without a key.
    bool Whole() const;

    // Packs Area() from `pixels` (the whole surface, rows `pitch` bytes apart) into `out`, rows
    // Area()'s width times `pixelBytes` apart: a marked pixel keeps its value, any other pixel
    // takes the returned key, the lowest value within `valueMask` that no marked pixel has.
    // `pixelBytes` is 2 or 4 and `valueMask` has more values than Area() has pixels.
    std::uint32_t Patch(const std::uint8_t *pixels, std::uint32_t pitch, std::uint32_t pixelBytes,
                        std::uint32_t valueMask, std::vector<std::uint8_t> &out) const;

private:
    bool marked(std::int32_t x, std::int32_t y) const {
        return marks_[static_cast<std::size_t>(y) * width_ + static_cast<std::size_t>(x)] != 0;
    }

    std::vector<std::uint8_t> marks_; // one byte per pixel, row by row
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint64_t count_ = 0;
    RectL area_;
};

} // namespace sfcop::hle::ddraw4
