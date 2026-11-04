// GuestShots: screenshots of the game's display (--shots DIR --shot-every S).
//
// The host reads the back buffer through Gpu::ReadTexture on the main thread and writes it as an
// uncompressed 24-bit BMP, the format src/hle/ddraw4/tools/framediff.py reads. The names carry the
// run's elapsed milliseconds, zero-padded, so two runs' folders sort in the same order.
//
// No lifted code in this file: it is built into the tests too.
#pragma once

#include "platform/Gpu/Gpu.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sfcop::guest {

// A 24-bit bottom-up BMP of an image of `width` by `height` pixels in `format`, rows `pitch`
// bytes apart. Empty for a format with no colour (indexed and depth) or a zero size.
std::vector<std::uint8_t> EncodeBmp(std::uint32_t width, std::uint32_t height,
                                    platform::PixelFormat format, const std::uint8_t *pixels,
                                    std::uint32_t pitch);

// Reads the whole texture (level 0) and encodes it. Errors: the Gpu's from ReadTexture();
// kInvalidArgument for a format EncodeBmp() refuses.
platform::Result<std::vector<std::uint8_t>> CaptureBmp(platform::Gpu &gpu,
                                                       platform::TextureId texture,
                                                       std::uint32_t width, std::uint32_t height,
                                                       platform::PixelFormat format);

// DIR/shot_<milliseconds, 9 digits>.bmp
std::string ShotPath(const std::string &directory, std::uint32_t milliseconds);

// Writes `bytes` to `path`, creating the directory. False on failure.
bool WriteShot(const std::string &path, const std::vector<std::uint8_t> &bytes);

// Decides when a shot is due: the first call that finds `interval` seconds elapsed since the last
// shot (the first shot is due at `interval`). A zero interval is never due.
class ShotSchedule {
public:
    explicit ShotSchedule(double intervalSeconds) : interval_(intervalSeconds), next_(intervalSeconds) {}
    bool Due(double nowSeconds);

private:
    double interval_;
    double next_;
};

} // namespace sfcop::guest
