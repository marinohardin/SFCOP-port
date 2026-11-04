// Pixel conversions for the SDL GPU backend. Every colour texture lives on the GPU as 8-bit B, G,
// R, A (the byte order of 0xAARRGGBB in memory); the game's formats are converted on the way in
// and out. Narrow channels widen by bit replication and narrow by truncation, so a value written
// in a 16-bit format reads back unchanged. An unused X byte reads back as 0xFF.
#include "platform/Gpu/Gpu.h"

#include <cstdint>
#include <cstring>

namespace sfcop::platform::gpusdl {
namespace {

std::uint8_t widen5(std::uint32_t v) { return static_cast<std::uint8_t>((v << 3) | (v >> 2)); }
std::uint8_t widen6(std::uint32_t v) { return static_cast<std::uint8_t>((v << 2) | (v >> 4)); }

std::uint32_t load16(const std::uint8_t *p) { return std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8); }
void store16(std::uint8_t *p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}

} // namespace

// The mask of the bits a raw pixel value of `format` may have.
std::uint32_t formatMask(PixelFormat format) {
    switch (format) {
    case PixelFormat::kRgb565:
    case PixelFormat::kArgb1555:
    case PixelFormat::kDepth16:
        return 0xFFFFu;
    case PixelFormat::kXrgb8888:
        return 0xFFFFFFu;
    case PixelFormat::kArgb8888:
        return 0xFFFFFFFFu;
    case PixelFormat::kIndexed8:
        return 0xFFu;
    }
    return 0;
}

// One raw pixel value as 0xAARRGGBB. An index reads through `palette` (black when null).
std::uint32_t rawToArgb(PixelFormat format, std::uint32_t raw, const PaletteEntry *palette) {
    switch (format) {
    case PixelFormat::kRgb565:
        return 0xFF000000u | (std::uint32_t{widen5((raw >> 11) & 31)} << 16) |
               (std::uint32_t{widen6((raw >> 5) & 63)} << 8) | widen5(raw & 31);
    case PixelFormat::kArgb1555:
        return ((raw & 0x8000u) != 0 ? 0xFF000000u : 0u) |
               (std::uint32_t{widen5((raw >> 10) & 31)} << 16) |
               (std::uint32_t{widen5((raw >> 5) & 31)} << 8) | widen5(raw & 31);
    case PixelFormat::kXrgb8888:
        return 0xFF000000u | (raw & 0xFFFFFFu);
    case PixelFormat::kArgb8888:
        return raw;
    case PixelFormat::kIndexed8: {
        if (palette == nullptr) {
            return 0xFF000000u;
        }
        const PaletteEntry &e = palette[raw & 0xFFu];
        return (std::uint32_t{e.alpha} << 24) | (std::uint32_t{e.red} << 16) |
               (std::uint32_t{e.green} << 8) | e.blue;
    }
    case PixelFormat::kDepth16:
        return 0;
    }
    return 0;
}

// 0xAARRGGBB as one raw pixel value of a direct-colour `format`.
std::uint32_t argbToRaw(PixelFormat format, std::uint32_t argb) {
    const std::uint32_t a = argb >> 24;
    const std::uint32_t r = (argb >> 16) & 0xFF;
    const std::uint32_t g = (argb >> 8) & 0xFF;
    const std::uint32_t b = argb & 0xFF;
    switch (format) {
    case PixelFormat::kRgb565:
        return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    case PixelFormat::kArgb1555:
        return ((a >= 128 ? 1u : 0u) << 15) | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
    case PixelFormat::kXrgb8888:
        return 0xFF000000u | (argb & 0xFFFFFFu);
    case PixelFormat::kArgb8888:
        return argb;
    case PixelFormat::kIndexed8:
    case PixelFormat::kDepth16:
        return 0;
    }
    return 0;
}

// `count` pixels of `format` at `src` to B, G, R, A bytes at `dst`.
void toBgra(PixelFormat format, const std::uint8_t *src, std::uint8_t *dst, std::uint32_t count,
            const PaletteEntry *palette) {
    const std::uint32_t bytes = pixelBytes(format);
    for (std::uint32_t i = 0; i < count; ++i, src += bytes, dst += 4) {
        std::uint32_t raw = 0;
        if (bytes == 1) {
            raw = src[0];
        } else if (bytes == 2) {
            raw = load16(src);
        } else {
            std::memcpy(&raw, src, 4);
        }
        const std::uint32_t argb = rawToArgb(format, raw, palette);
        std::memcpy(dst, &argb, 4);
    }
}

// `count` B, G, R, A pixels at `src` to a direct-colour `format` at `dst`.
void fromBgra(PixelFormat format, const std::uint8_t *src, std::uint8_t *dst, std::uint32_t count) {
    const std::uint32_t bytes = pixelBytes(format);
    for (std::uint32_t i = 0; i < count; ++i, src += 4, dst += bytes) {
        std::uint32_t argb = 0;
        std::memcpy(&argb, src, 4);
        const std::uint32_t raw = argbToRaw(format, argb);
        if (bytes == 2) {
            store16(dst, raw);
        } else {
            std::memcpy(dst, &raw, 4);
        }
    }
}

} // namespace sfcop::platform::gpusdl
