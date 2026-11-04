// Guest memory access for the DirectDraw and Direct3D guest shims. Included only by *Guest.cpp
// files: it needs the lift's headers (src/hle/README.md, "Implementing a module").
//
// Every structure the game passes is read and written field by field at its documented offsets;
// no host structure is laid over guest memory. A structure that carries a dwSize is written only
// up to the size the game gave.
#pragma once

extern "C" {
#include "win32hle.h"
}

#include "hle/ddraw4/Ddraw4Defs.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string_view>

namespace sfcop::hle::ddraw4::guest {

inline std::uint32_t read32(std::uint32_t address) {
    return MEM32(address);
}
inline void write32(std::uint32_t address, std::uint32_t value) {
    MEM32(address) = value;
}
inline float readFloat(std::uint32_t address) {
    return MEMF(address);
}
inline void writeFloat(std::uint32_t address, float value) {
    MEMF(address) = value;
}

// Writes `value` through an out-parameter the game may have left null.
inline void writeOut(std::uint32_t address, std::uint32_t value) {
    if (address != 0) {
        write32(address, value);
    }
}

inline void zero(std::uint32_t address, std::uint32_t bytes) {
    for (std::uint32_t i = 0; i < bytes; ++i) {
        MEM8(address + i) = 0;
    }
}

inline void copyOut(std::uint32_t address, const void *bytes, std::uint32_t count) {
    const auto *from = static_cast<const std::uint8_t *>(bytes);
    for (std::uint32_t i = 0; i < count; ++i) {
        MEM8(address + i) = from[i];
    }
}

inline void copyIn(void *bytes, std::uint32_t address, std::uint32_t count) {
    auto *to = static_cast<std::uint8_t *>(bytes);
    for (std::uint32_t i = 0; i < count; ++i) {
        to[i] = MEM8(address + i);
    }
}

// A NUL-terminated copy of `text` in a field of `size` bytes, cut to fit.
inline void writeString(std::uint32_t address, std::uint32_t size, std::string_view text) {
    const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(text.size(), size - 1));
    copyOut(address, text.data(), count);
    MEM8(address + count) = 0;
}

inline Guid readGuid(std::uint32_t address) {
    Guid guid{};
    copyIn(guid.data(), address, 16);
    return guid;
}

inline void writeGuid(std::uint32_t address, const Guid &guid) {
    copyOut(address, guid.data(), 16);
}

// RECT.
inline RectL readRect(std::uint32_t address) {
    return RectL{static_cast<std::int32_t>(read32(address)),
                 static_cast<std::int32_t>(read32(address + 4)),
                 static_cast<std::int32_t>(read32(address + 8)),
                 static_cast<std::int32_t>(read32(address + 12))};
}

// DDCOLORKEY.
inline ColorKey readKey(std::uint32_t address) {
    return ColorKey{read32(address), read32(address + 4)};
}

inline void writeKey(std::uint32_t address, const ColorKey &key) {
    write32(address, key.low);
    write32(address + 4, key.high);
}

// DDPIXELFORMAT: dwSize, dwFlags, dwFourCC, dwRGBBitCount, the four masks (32 bytes).
constexpr std::uint32_t kPixelFormatSize = 32;

inline PixelFormatDesc readPixelFormat(std::uint32_t address) {
    PixelFormatDesc format;
    format.flags = read32(address + 4);
    format.fourCc = read32(address + 8);
    format.bitCount = read32(address + 12);
    format.redMask = read32(address + 16);
    format.greenMask = read32(address + 20);
    format.blueMask = read32(address + 24);
    format.alphaMask = read32(address + 28);
    return format;
}

inline void writePixelFormat(std::uint32_t address, const PixelFormatDesc &format) {
    write32(address, kPixelFormatSize);
    write32(address + 4, format.flags);
    write32(address + 8, format.fourCc);
    write32(address + 12, format.bitCount);
    write32(address + 16, format.redMask);
    write32(address + 20, format.greenMask);
    write32(address + 24, format.blueMask);
    write32(address + 28, format.alphaMask);
}

// DDSCAPS (one word) or DDSCAPS2 (four).
inline SurfaceCaps readCaps(std::uint32_t address, bool caps2) {
    SurfaceCaps caps;
    caps.caps = read32(address);
    if (caps2) {
        caps.caps2 = read32(address + 4);
        caps.caps3 = read32(address + 8);
        caps.caps4 = read32(address + 12);
    }
    return caps;
}

inline void writeCaps(std::uint32_t address, const SurfaceCaps &caps, bool caps2) {
    write32(address, caps.caps);
    if (caps2) {
        write32(address + 4, caps.caps2);
        write32(address + 8, caps.caps3);
        write32(address + 12, caps.caps4);
    }
}

// DDSURFACEDESC (108 bytes, a DDSCAPS at 104) and DDSURFACEDESC2 (124 bytes, a DDSCAPS2 at 104
// and dwTextureStage at 120); the fields before 104 are the same.
constexpr std::uint32_t kSurfaceDescSize = 108;
constexpr std::uint32_t kSurfaceDesc2Size = 124;

inline bool isSurfaceDescSize(std::uint32_t size) {
    return size == kSurfaceDescSize || size == kSurfaceDesc2Size;
}

// The description at `address` in the form its dwSize says, or std::nullopt for another size.
inline std::optional<SurfaceDesc> readSurfaceDesc(std::uint32_t address) {
    const std::uint32_t size = read32(address);
    if (!isSurfaceDescSize(size)) {
        return std::nullopt;
    }
    SurfaceDesc desc;
    desc.flags = read32(address + 4);
    desc.height = read32(address + 8);
    desc.width = read32(address + 12);
    desc.pitch = static_cast<std::int32_t>(read32(address + 16));
    desc.backBufferCount = read32(address + 20);
    desc.mipMapCount = read32(address + 24);
    desc.alphaBitDepth = read32(address + 28);
    desc.surface = static_cast<std::uint8_t *>(hle_ptr(read32(address + 36)));
    desc.destOverlay = readKey(address + 40);
    desc.destBlt = readKey(address + 48);
    desc.srcOverlay = readKey(address + 56);
    desc.srcBlt = readKey(address + 64);
    desc.pixelFormat = readPixelFormat(address + 72);
    desc.caps = readCaps(address + 104, size == kSurfaceDesc2Size);
    if (size == kSurfaceDesc2Size) {
        desc.textureStage = read32(address + 120);
    }
    return desc;
}

// Writes `desc` in the form of `size` (one of the two above), dwSize included.
inline void writeSurfaceDesc(std::uint32_t address, const SurfaceDesc &desc, std::uint32_t size) {
    zero(address, size);
    write32(address, size);
    write32(address + 4, desc.flags);
    write32(address + 8, desc.height);
    write32(address + 12, desc.width);
    write32(address + 16, static_cast<std::uint32_t>(desc.pitch));
    write32(address + 20, desc.backBufferCount);
    write32(address + 24, desc.mipMapCount);
    write32(address + 28, desc.alphaBitDepth);
    write32(address + 36, desc.surface == nullptr ? 0u : hle_va(desc.surface));
    writeKey(address + 40, desc.destOverlay);
    writeKey(address + 48, desc.destBlt);
    writeKey(address + 56, desc.srcOverlay);
    writeKey(address + 64, desc.srcBlt);
    writePixelFormat(address + 72, desc.pixelFormat);
    writeCaps(address + 104, desc.caps, size == kSurfaceDesc2Size);
    if (size == kSurfaceDesc2Size) {
        write32(address + 120, desc.textureStage);
    }
}

// PALETTEENTRY: red, green, blue, flags.
inline PaletteColor readPaletteColor(std::uint32_t address) {
    return PaletteColor{MEM8(address), MEM8(address + 1), MEM8(address + 2), MEM8(address + 3)};
}

inline void writePaletteColor(std::uint32_t address, const PaletteColor &color) {
    MEM8(address) = color.red;
    MEM8(address + 1) = color.green;
    MEM8(address + 2) = color.blue;
    MEM8(address + 3) = color.flags;
}

// Calls a guest function (an enumeration callback) with stdcall arguments; returns its eax.
inline std::uint32_t callGuest(std::uint32_t function, std::initializer_list<std::uint32_t> args) {
    std::uint32_t values[8] = {};
    int count = 0;
    for (const std::uint32_t arg : args) {
        values[count++] = arg;
    }
    return hle_call_guest(function, count, values);
}

// Guest memory for what a callback is handed, released at the end of the scope.
class Scratch {
public:
    Scratch() : mark_(hle_scratch_mark()) {}
    ~Scratch() { hle_scratch_release(mark_); }
    Scratch(const Scratch &) = delete;
    Scratch &operator=(const Scratch &) = delete;

    // `bytes` zeroed bytes.
    std::uint32_t Take(std::uint32_t bytes) { return hle_scratch(bytes); }

    std::uint32_t Text(std::string_view text) {
        const std::uint32_t address = Take(static_cast<std::uint32_t>(text.size()) + 1);
        writeString(address, static_cast<std::uint32_t>(text.size()) + 1, text);
        return address;
    }

private:
    std::uint32_t mark_;
};

} // namespace sfcop::hle::ddraw4::guest
