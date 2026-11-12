#include "hle/ddraw4/ChangedPixels.h"

#include <algorithm>
#include <cstring>

namespace sfcop::hle::ddraw4 {

namespace {

std::uint32_t readPixel(const std::uint8_t *at, std::uint32_t pixelBytes) {
    if (pixelBytes == 2) {
        std::uint16_t value = 0;
        std::memcpy(&value, at, sizeof value);
        return value;
    }
    std::uint32_t value = 0;
    std::memcpy(&value, at, sizeof value);
    return value;
}

void writePixel(std::uint8_t *at, std::uint32_t pixelBytes, std::uint32_t value) {
    if (pixelBytes == 2) {
        const auto narrow = static_cast<std::uint16_t>(value);
        std::memcpy(at, &narrow, sizeof narrow);
        return;
    }
    std::memcpy(at, &value, sizeof value);
}

} // namespace

void ChangedPixels::Reset(std::uint32_t width, std::uint32_t height) {
    width_ = width;
    height_ = height;
    marks_.assign(static_cast<std::size_t>(width) * height, 0);
    count_ = 0;
    area_ = RectL{};
}

void ChangedPixels::Clear() {
    if (count_ == 0) {
        return;
    }
    for (std::int32_t y = area_.top; y < area_.bottom; ++y) {
        std::uint8_t *row = marks_.data() + static_cast<std::size_t>(y) * width_;
        std::fill(row + area_.left, row + area_.right, std::uint8_t{0});
    }
    count_ = 0;
    area_ = RectL{};
}

void ChangedPixels::Compare(const RectL &area, const std::uint8_t *before,
                            std::uint32_t beforePitch, const std::uint8_t *after,
                            std::uint32_t afterPitch, std::uint32_t pixelBytes) {
    const auto rowBytes = static_cast<std::size_t>(area.right - area.left) * pixelBytes;
    for (std::int32_t y = area.top; y < area.bottom; ++y) {
        const std::uint8_t *old = before + static_cast<std::size_t>(y - area.top) * beforePitch;
        const std::uint8_t *now = after + static_cast<std::size_t>(y - area.top) * afterPitch;
        if (std::memcmp(old, now, rowBytes) == 0) {
            continue; // most rows of a frame are untouched
        }
        std::uint8_t *marks = marks_.data() + static_cast<std::size_t>(y) * width_;
        for (std::int32_t x = area.left; x < area.right; ++x) {
            const std::size_t at = static_cast<std::size_t>(x - area.left) * pixelBytes;
            if (std::memcmp(old + at, now + at, pixelBytes) == 0 || marks[x] != 0) {
                continue;
            }
            marks[x] = 1;
            if (count_ == 0) {
                area_ = RectL{x, y, x + 1, y + 1};
            } else {
                area_.left = std::min(area_.left, x);
                area_.top = std::min(area_.top, y);
                area_.right = std::max(area_.right, x + 1);
                area_.bottom = std::max(area_.bottom, y + 1);
            }
            ++count_;
        }
    }
}

bool ChangedPixels::Whole() const {
    const auto pixels = static_cast<std::uint64_t>(area_.right - area_.left) *
                        static_cast<std::uint64_t>(area_.bottom - area_.top);
    return count_ != 0 && count_ == pixels;
}

std::uint32_t ChangedPixels::Patch(const std::uint8_t *pixels, std::uint32_t pitch,
                                   std::uint32_t pixelBytes, std::uint32_t valueMask,
                                   std::vector<std::uint8_t> &out) const {
    const auto width = static_cast<std::size_t>(area_.right - area_.left);
    const auto height = static_cast<std::size_t>(area_.bottom - area_.top);
    // The key: the lowest value no marked pixel has, found from the sorted marked values.
    std::vector<std::uint32_t> used;
    used.reserve(static_cast<std::size_t>(count_));
    for (std::int32_t y = area_.top; y < area_.bottom; ++y) {
        const std::uint8_t *row = pixels + static_cast<std::size_t>(y) * pitch;
        for (std::int32_t x = area_.left; x < area_.right; ++x) {
            if (marked(x, y)) {
                used.push_back(readPixel(row + static_cast<std::size_t>(x) * pixelBytes,
                                         pixelBytes) &
                               valueMask);
            }
        }
    }
    std::sort(used.begin(), used.end());
    used.erase(std::unique(used.begin(), used.end()), used.end());
    std::uint32_t key = 0;
    for (const std::uint32_t value : used) {
        if (value != key) {
            break;
        }
        ++key;
    }
    out.resize(width * height * pixelBytes);
    for (std::int32_t y = area_.top; y < area_.bottom; ++y) {
        const std::uint8_t *row = pixels + static_cast<std::size_t>(y) * pitch;
        std::uint8_t *to = out.data() + static_cast<std::size_t>(y - area_.top) * width * pixelBytes;
        for (std::int32_t x = area_.left; x < area_.right; ++x) {
            const std::size_t at = static_cast<std::size_t>(x) * pixelBytes;
            std::uint8_t *pixel = to + static_cast<std::size_t>(x - area_.left) * pixelBytes;
            if (marked(x, y)) {
                std::memcpy(pixel, row + at, pixelBytes);
            } else {
                writePixel(pixel, pixelBytes, key);
            }
        }
    }
    return key;
}

} // namespace sfcop::hle::ddraw4
