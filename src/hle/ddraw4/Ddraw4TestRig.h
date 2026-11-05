// What the DirectDraw doctests share (tests only): a DirectDraw object on GpuMock and WindowMock,
// a record of destroyed objects, and the surface descriptions the game passes.
#pragma once

#include "hle/ddraw4/DirectDraw.h"
#include "hle/ddraw4/Surface.h"

#include "platform/Gpu/GpuMock.h"
#include "platform/Window/WindowMock.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace sfcop::hle::ddraw4::test {

using Calls = std::vector<std::string>;

inline std::vector<const ComObject *> &destroyed() {
    static std::vector<const ComObject *> objects;
    return objects;
}

inline void recordDestroyed(const ComObject *object) {
    destroyed().push_back(object);
}

inline bool wasDestroyed(const ComObject *object) {
    return std::find(destroyed().begin(), destroyed().end(), object) != destroyed().end();
}

struct Rig {
    platform::GpuMock gpu;
    platform::WindowMock window;
    HeapPixelMemory memory;
    DirectDraw *dd = nullptr;

    Rig() {
        destroyed().clear();
        DirectDraw::ForgetDisplayOwners();
        ComObject::SetDestroyHook(&recordDestroyed);
        dd = new DirectDraw(gpu, window, memory); // the reference DirectDrawCreate hands out
    }
    ~Rig() {
        if (!wasDestroyed(dd)) {
            dd->Release();
        }
        ComObject::SetDestroyHook(nullptr);
    }

    Surface *Create(const SurfaceDesc &desc) {
        Surface *surface = nullptr;
        REQUIRE(dd->CreateSurface(desc, &surface) == kDdOk);
        REQUIRE(surface != nullptr);
        return surface;
    }
};

// An offscreen surface as the game asks for one: caps, size and pixel format.
inline SurfaceDesc plainDesc(std::uint32_t width, std::uint32_t height,
                             platform::PixelFormat format) {
    SurfaceDesc desc;
    desc.flags = kDdsdCaps | kDdsdWidth | kDdsdHeight | kDdsdPixelFormat;
    desc.width = width;
    desc.height = height;
    desc.caps.caps = kDdscapsOffscreenPlain;
    desc.pixelFormat = describe(format);
    return desc;
}

// The game's main primary: a flipping chain with one back buffer that Direct3D draws into.
inline SurfaceDesc flipChainDesc() {
    SurfaceDesc desc;
    desc.flags = kDdsdCaps | kDdsdBackBufferCount;
    desc.caps.caps = kDdscapsPrimarySurface | kDdscapsFlip | kDdscapsComplex | kDdscaps3dDevice;
    desc.backBufferCount = 1;
    return desc;
}

// A primary without a back buffer.
inline SurfaceDesc primaryDesc() {
    SurfaceDesc desc;
    desc.flags = kDdsdCaps;
    desc.caps.caps = kDdscapsPrimarySurface;
    return desc;
}

// Writes 16-bit pixels into a locked surface, row after row, and unlocks it.
inline void writePixels16(Surface &surface, const std::vector<std::uint16_t> &pixels) {
    SurfaceDesc locked;
    REQUIRE(surface.Lock(nullptr, locked, 0) == kDdOk);
    for (std::uint32_t y = 0; y < locked.height; ++y) {
        std::memcpy(locked.surface + static_cast<std::size_t>(y) * locked.pitch,
                    pixels.data() + static_cast<std::size_t>(y) * locked.width, locked.width * 2u);
    }
    REQUIRE(surface.Unlock() == kDdOk);
}

// Reads a locked surface's 16-bit pixels, row after row.
inline std::vector<std::uint16_t> readPixels16(Surface &surface) {
    SurfaceDesc locked;
    REQUIRE(surface.Lock(nullptr, locked, kDdlockReadOnly) == kDdOk);
    std::vector<std::uint16_t> pixels(static_cast<std::size_t>(locked.width) * locked.height);
    for (std::uint32_t y = 0; y < locked.height; ++y) {
        std::memcpy(pixels.data() + static_cast<std::size_t>(y) * locked.width,
                    locked.surface + static_cast<std::size_t>(y) * locked.pitch, locked.width * 2u);
    }
    REQUIRE(surface.Unlock() == kDdOk);
    return pixels;
}

} // namespace sfcop::hle::ddraw4::test
