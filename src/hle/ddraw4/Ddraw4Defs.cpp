#include "hle/ddraw4/Ddraw4Defs.h"
#include "hle/ddraw4/PixelMemory.h"

#include <cstdlib>

namespace sfcop::hle::ddraw4 {

using platform::Error;
using platform::PixelFormat;

std::uint8_t *HeapPixelMemory::Allocate(std::size_t bytes) {
    return static_cast<std::uint8_t *>(std::calloc(bytes == 0 ? 1 : bytes, 1));
}

void HeapPixelMemory::Free(std::uint8_t *block) {
    std::free(block);
}

std::optional<PixelFormat> formatOf(const PixelFormatDesc &desc) {
    if ((desc.flags & kDdpfZBuffer) != 0) {
        if (desc.bitCount == 16) {
            return PixelFormat::kDepth16;
        }
        return std::nullopt;
    }
    if ((desc.flags & kDdpfPaletteIndexed8) != 0) {
        return PixelFormat::kIndexed8;
    }
    if ((desc.flags & kDdpfRgb) == 0 || (desc.flags & kDdpfFourCc) != 0) {
        return std::nullopt;
    }
    const bool alpha = (desc.flags & kDdpfAlphaPixels) != 0;
    if (desc.bitCount == 16 && desc.redMask == 0xF800 && desc.greenMask == 0x07E0 &&
        desc.blueMask == 0x001F && !alpha) {
        return PixelFormat::kRgb565;
    }
    // 555 without alpha is stored as 1555; the unused top bit is the alpha the Gpu ignores
    // unless a pipeline blends with it.
    if (desc.bitCount == 16 && desc.redMask == 0x7C00 && desc.greenMask == 0x03E0 &&
        desc.blueMask == 0x001F && (!alpha || desc.alphaMask == 0x8000)) {
        return PixelFormat::kArgb1555;
    }
    if (desc.bitCount == 32 && desc.redMask == 0x00FF0000 && desc.greenMask == 0x0000FF00 &&
        desc.blueMask == 0x000000FF) {
        if (!alpha) {
            return PixelFormat::kXrgb8888;
        }
        if (desc.alphaMask == 0xFF000000) {
            return PixelFormat::kArgb8888;
        }
    }
    return std::nullopt;
}

PixelFormatDesc describe(PixelFormat format) {
    PixelFormatDesc desc;
    switch (format) {
    case PixelFormat::kRgb565:
        desc = {kDdpfRgb, 0, 16, 0xF800, 0x07E0, 0x001F, 0};
        break;
    case PixelFormat::kArgb1555:
        desc = {kDdpfRgb | kDdpfAlphaPixels, 0, 16, 0x7C00, 0x03E0, 0x001F, 0x8000};
        break;
    case PixelFormat::kXrgb8888:
        desc = {kDdpfRgb, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0};
        break;
    case PixelFormat::kArgb8888:
        desc = {kDdpfRgb | kDdpfAlphaPixels, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000};
        break;
    case PixelFormat::kIndexed8:
        desc = {kDdpfRgb | kDdpfPaletteIndexed8, 0, 8, 0, 0, 0, 0};
        break;
    case PixelFormat::kDepth16:
        desc = {kDdpfZBuffer, 0, 16, 0, 0xFFFF, 0, 0};
        break;
    }
    return desc;
}

std::optional<PixelFormat> displayFormat(std::uint32_t bitsPerPixel) {
    switch (bitsPerPixel) {
    case 8:
        return PixelFormat::kIndexed8;
    case 16:
        return PixelFormat::kRgb565;
    case 32:
        return PixelFormat::kXrgb8888;
    default:
        return std::nullopt;
    }
}

std::uint32_t packColor(PixelFormat format, std::uint32_t argb) {
    const std::uint32_t a = argb >> 24;
    const std::uint32_t r = (argb >> 16) & 0xFF;
    const std::uint32_t g = (argb >> 8) & 0xFF;
    const std::uint32_t b = argb & 0xFF;
    switch (format) {
    case PixelFormat::kRgb565:
        return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    case PixelFormat::kArgb1555:
        return ((a >> 7) << 15) | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
    case PixelFormat::kXrgb8888:
        return argb & 0x00FFFFFF;
    case PixelFormat::kArgb8888:
        return argb;
    case PixelFormat::kIndexed8:
        return b;
    case PixelFormat::kDepth16:
        return 0xFFFF;
    }
    return 0;
}

HResult fromError(Error error) {
    switch (error) {
    case Error::kNone:
        return kDdOk;
    case Error::kInvalidArgument:
        return kDdErrInvalidParams;
    case Error::kInvalidHandle:
        return kDdErrInvalidObject;
    case Error::kOutOfResources:
        return kDdErrOutOfVideoMemory;
    case Error::kUnsupported:
        return kDdErrUnsupported;
    case Error::kWrongState:
        return kDdErrSurfaceBusy;
    case Error::kIoError:
        return kDdErrSurfaceLost;
    default:
        return kDdErrGeneric;
    }
}

} // namespace sfcop::hle::ddraw4
