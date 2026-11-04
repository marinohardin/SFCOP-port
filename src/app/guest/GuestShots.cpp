#include "GuestShots.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace sfcop::guest {

namespace {

void put16(std::vector<std::uint8_t> &out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void put32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    put16(out, value & 0xFFFFu);
    put16(out, value >> 16);
}

std::uint8_t expand(std::uint32_t value, unsigned bits) { // a 5- or 6-bit channel to 8 bits
    return static_cast<std::uint8_t>((value << (8 - bits)) | (value >> (2 * bits - 8)));
}

} // namespace

std::vector<std::uint8_t> EncodeBmp(std::uint32_t width, std::uint32_t height,
                                    platform::PixelFormat format, const std::uint8_t *pixels,
                                    std::uint32_t pitch) {
    using platform::PixelFormat;
    if (width == 0 || height == 0 || pixels == nullptr || format == PixelFormat::kIndexed8 ||
        format == PixelFormat::kDepth16) {
        return {};
    }
    const std::uint32_t stride = (width * 3 + 3) & ~3u;
    const std::uint32_t imageBytes = stride * height;
    std::vector<std::uint8_t> out;
    out.reserve(54 + imageBytes);
    out.push_back('B');
    out.push_back('M');
    put32(out, 54 + imageBytes);
    put32(out, 0);
    put32(out, 54);
    put32(out, 40); // BITMAPINFOHEADER
    put32(out, width);
    put32(out, height); // positive: bottom-up
    put16(out, 1);
    put16(out, 24);
    put32(out, 0); // BI_RGB
    put32(out, imageBytes);
    put32(out, 2835); // 72 dpi
    put32(out, 2835);
    put32(out, 0);
    put32(out, 0);
    for (std::uint32_t y = height; y > 0; --y) {
        const std::uint8_t *row = pixels + static_cast<std::size_t>(y - 1) * pitch;
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint8_t blue = 0, green = 0, red = 0;
            if (format == PixelFormat::kXrgb8888 || format == PixelFormat::kArgb8888) {
                blue = row[4 * x];
                green = row[4 * x + 1];
                red = row[4 * x + 2];
            } else {
                const std::uint32_t value = row[2 * x] | (static_cast<std::uint32_t>(row[2 * x + 1]) << 8);
                if (format == PixelFormat::kRgb565) {
                    red = expand((value >> 11) & 31, 5);
                    green = expand((value >> 5) & 63, 6);
                    blue = expand(value & 31, 5);
                } else { // kArgb1555
                    red = expand((value >> 10) & 31, 5);
                    green = expand((value >> 5) & 31, 5);
                    blue = expand(value & 31, 5);
                }
            }
            out.push_back(blue);
            out.push_back(green);
            out.push_back(red);
        }
        for (std::uint32_t pad = width * 3; pad < stride; ++pad) {
            out.push_back(0);
        }
    }
    return out;
}

platform::Result<std::vector<std::uint8_t>> CaptureBmp(platform::Gpu &gpu,
                                                       platform::TextureId texture,
                                                       std::uint32_t width, std::uint32_t height,
                                                       platform::PixelFormat format) {
    const std::uint32_t pitch = width * platform::pixelBytes(format);
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(pitch) * height);
    const platform::Status read =
        gpu.ReadTexture(texture, 0, platform::Rect{0, 0, width, height}, pixels.data(), pitch);
    if (!read.Ok()) {
        return read.Code();
    }
    std::vector<std::uint8_t> bmp = EncodeBmp(width, height, format, pixels.data(), pitch);
    if (bmp.empty()) {
        return platform::Error::kInvalidArgument;
    }
    return bmp;
}

std::string ShotPath(const std::string &directory, std::uint32_t milliseconds) {
    char name[32];
    std::snprintf(name, sizeof name, "shot_%09u.bmp", milliseconds);
    return (std::filesystem::path(directory) / name).string();
}

bool WriteShot(const std::string &path, const std::vector<std::uint8_t> &bytes) {
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    return file.good();
}

bool ShotSchedule::Due(double nowSeconds) {
    if (interval_ <= 0 || nowSeconds < next_) {
        return false;
    }
    next_ = nowSeconds + interval_;
    return true;
}

} // namespace sfcop::guest
