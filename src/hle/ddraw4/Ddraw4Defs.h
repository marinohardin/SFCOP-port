// The DirectDraw constants and value types the module works with (DESIGN §6.1): return codes,
// flags, and host-side copies of the structures the game passes by pointer. Numeric values are
// those of the public header include/ddraw.h of Wine 9.0. The guest shims (Ddraw4Guest.cpp) read
// and write the game's structures field by field into these types; nothing here is laid over
// guest memory.
#pragma once

#include "platform/Core/Result.h"
#include "platform/Gpu/Gpu.h"

#include <array>
#include <cstdint>
#include <optional>

namespace sfcop::hle::ddraw4 {

// An HRESULT as the game sees it: the 32 bits it tests, unsigned so that the codes compare as
// written.
using HResult = std::uint32_t;

constexpr HResult ddHresult(std::uint32_t code) {
    return 0x88760000u | code;
}

constexpr HResult kDdOk = 0;
constexpr HResult kDdFalse = 1;                      // S_FALSE
constexpr HResult kDdErrGeneric = 0x80004005u;       // E_FAIL
constexpr HResult kDdErrInvalidParams = 0x80070057u; // E_INVALIDARG
constexpr HResult kDdErrUnsupported = 0x80004001u;   // E_NOTIMPL
constexpr HResult kDdErrOutOfMemory = 0x8007000Eu;   // E_OUTOFMEMORY
constexpr HResult kENoInterface = 0x80004002u;
constexpr HResult kClassENoAggregation = 0x80040110u;
constexpr HResult kDdErrAlreadyInitialized = ddHresult(5);
constexpr HResult kDdErrCannotAttachSurface = ddHresult(10);
constexpr HResult kDdErrCannotDetachSurface = ddHresult(20);
constexpr HResult kDdErrInvalidCaps = ddHresult(100);
constexpr HResult kDdErrInvalidMode = ddHresult(120);
constexpr HResult kDdErrInvalidObject = ddHresult(130);
constexpr HResult kDdErrInvalidPixelFormat = ddHresult(145);
constexpr HResult kDdErrInvalidRect = ddHresult(150);
constexpr HResult kDdErrNoClipList = ddHresult(205);
constexpr HResult kDdErrNoColorKey = ddHresult(215);
constexpr HResult kDdErrNotFound = ddHresult(255);
constexpr HResult kDdErrOutOfVideoMemory = ddHresult(380);
constexpr HResult kDdErrSurfaceAlreadyAttached = ddHresult(410);
constexpr HResult kDdErrSurfaceBusy = ddHresult(430);
constexpr HResult kDdErrSurfaceLost = ddHresult(450);
constexpr HResult kDdErrInvalidDirectDrawGuid = ddHresult(561);
constexpr HResult kDdErrRegionTooSmall = ddHresult(566);
constexpr HResult kDdErrNoClipperAttached = ddHresult(568);
constexpr HResult kDdErrNoPaletteAttached = ddHresult(572);
constexpr HResult kDdErrNotFlippable = ddHresult(582);
constexpr HResult kDdErrNotLocked = ddHresult(584);
constexpr HResult kDdErrNoDc = ddHresult(586);
constexpr HResult kDdErrNotPalettized = ddHresult(589);
constexpr HResult kDdErrDcAlreadyCreated = ddHresult(620);
constexpr HResult kDdErrMoreData = ddHresult(690);

// DDSURFACEDESC(2).dwFlags
constexpr std::uint32_t kDdsdCaps = 0x00000001;
constexpr std::uint32_t kDdsdHeight = 0x00000002;
constexpr std::uint32_t kDdsdWidth = 0x00000004;
constexpr std::uint32_t kDdsdPitch = 0x00000008;
constexpr std::uint32_t kDdsdBackBufferCount = 0x00000020;
constexpr std::uint32_t kDdsdZBufferBitDepth = 0x00000040;
constexpr std::uint32_t kDdsdLpSurface = 0x00000800;
constexpr std::uint32_t kDdsdPixelFormat = 0x00001000;
constexpr std::uint32_t kDdsdCkDestBlt = 0x00004000;
constexpr std::uint32_t kDdsdCkSrcBlt = 0x00010000;
constexpr std::uint32_t kDdsdMipMapCount = 0x00020000;
constexpr std::uint32_t kDdsdRefreshRate = 0x00040000;

// DDSCAPS.dwCaps
constexpr std::uint32_t kDdscapsBackBuffer = 0x00000004;
constexpr std::uint32_t kDdscapsComplex = 0x00000008;
constexpr std::uint32_t kDdscapsFlip = 0x00000010;
constexpr std::uint32_t kDdscapsFrontBuffer = 0x00000020;
constexpr std::uint32_t kDdscapsOffscreenPlain = 0x00000040;
constexpr std::uint32_t kDdscapsOverlay = 0x00000080;
constexpr std::uint32_t kDdscapsPalette = 0x00000100;
constexpr std::uint32_t kDdscapsPrimarySurface = 0x00000200;
constexpr std::uint32_t kDdscapsSystemMemory = 0x00000800;
constexpr std::uint32_t kDdscapsTexture = 0x00001000;
constexpr std::uint32_t kDdscaps3dDevice = 0x00002000;
constexpr std::uint32_t kDdscapsVideoMemory = 0x00004000;
constexpr std::uint32_t kDdscapsVisible = 0x00008000;
constexpr std::uint32_t kDdscapsZBuffer = 0x00020000;
constexpr std::uint32_t kDdscapsMipMap = 0x00400000;
constexpr std::uint32_t kDdscapsLocalVidMem = 0x10000000;

// DDPIXELFORMAT.dwFlags
constexpr std::uint32_t kDdpfAlphaPixels = 0x00000001;
constexpr std::uint32_t kDdpfFourCc = 0x00000004;
constexpr std::uint32_t kDdpfPaletteIndexed8 = 0x00000020;
constexpr std::uint32_t kDdpfRgb = 0x00000040;
constexpr std::uint32_t kDdpfZBuffer = 0x00000400;

// Blt, BltFast, Lock, Flip, SetColorKey, SetCooperativeLevel flags.
constexpr std::uint32_t kDdbltColorFill = 0x00000400;
constexpr std::uint32_t kDdbltDdfx = 0x00000800;
constexpr std::uint32_t kDdbltKeyDest = 0x00002000;
constexpr std::uint32_t kDdbltKeyDestOverride = 0x00004000;
constexpr std::uint32_t kDdbltKeySrc = 0x00008000;
constexpr std::uint32_t kDdbltKeySrcOverride = 0x00010000;
constexpr std::uint32_t kDdbltRop = 0x00020000;
constexpr std::uint32_t kDdbltfastSrcColorKey = 0x00000001;
constexpr std::uint32_t kDdbltfastDestColorKey = 0x00000002;
constexpr std::uint32_t kDdlockReadOnly = 0x00000010;
constexpr std::uint32_t kDdckeyDestBlt = 0x00000002;
constexpr std::uint32_t kDdckeySrcBlt = 0x00000008;
constexpr std::uint32_t kDdckeyColorSpace = 0x00000001;
constexpr std::uint32_t kDdsclFullscreen = 0x00000001;
constexpr std::uint32_t kDdsclNormal = 0x00000008;
constexpr std::uint32_t kDdsclExclusive = 0x00000010;

// Raster operations Blt accepts with DDBLT_ROP.
constexpr std::uint32_t kRopSrcCopy = 0x00CC0020;
constexpr std::uint32_t kRopBlackness = 0x00000042;
constexpr std::uint32_t kRopWhiteness = 0x00FF0062;

// Capability bits (DDCAPS, DDPCAPS, DDBD).
constexpr std::uint32_t kDdcaps3d = 0x00000001;
constexpr std::uint32_t kDdcapsBlt = 0x00000040;
constexpr std::uint32_t kDdcapsBltStretch = 0x00000200;
constexpr std::uint32_t kDdcapsPalette = 0x00008000;
constexpr std::uint32_t kDdcapsColorKey = 0x00400000;
constexpr std::uint32_t kDdcapsBltColorFill = 0x04000000;
constexpr std::uint32_t kDdcaps2WideSurfaces = 0x00001000;
constexpr std::uint32_t kDdcaps2CanRenderWindowed = 0x00080000;
constexpr std::uint32_t kDdckeycapsSrcBlt = 0x00000200;
constexpr std::uint32_t kDdfxcapsBltStretchAndShrink =
    0x00000400 | 0x00001000 | 0x00004000 | 0x00010000;
constexpr std::uint32_t kDdpcaps8Bit = 0x00000004;
constexpr std::uint32_t kDdpcapsPrimarySurface = 0x00000010;
constexpr std::uint32_t kDdpcapsAllow256 = 0x00000040;
constexpr std::uint32_t kDdbd8 = 0x00000800;
constexpr std::uint32_t kDdbd16 = 0x00000400;
constexpr std::uint32_t kDdbd32 = 0x00000100;

constexpr std::uint32_t kDdenumretCancel = 0;

// A Windows RECT: right and bottom are exclusive.
struct RectL {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
};

// DDCOLORKEY: the inclusive range of raw pixel values that are transparent.
struct ColorKey {
    std::uint32_t low = 0;
    std::uint32_t high = 0;
};

// DDPIXELFORMAT without its dwSize.
struct PixelFormatDesc {
    std::uint32_t flags = 0;
    std::uint32_t fourCc = 0;
    std::uint32_t bitCount = 0; // also the z-buffer depth
    std::uint32_t redMask = 0;  // a z-buffer's dwZBitMask is in greenMask (DDPIXELFORMAT's union)
    std::uint32_t greenMask = 0;
    std::uint32_t blueMask = 0;
    std::uint32_t alphaMask = 0;
};

// DDSCAPS2 (a DDSCAPS is `caps` alone).
struct SurfaceCaps {
    std::uint32_t caps = 0;
    std::uint32_t caps2 = 0;
    std::uint32_t caps3 = 0;
    std::uint32_t caps4 = 0;
};

// DDSURFACEDESC2 without its dwSize; a DDSURFACEDESC is the same fields with caps.caps alone and
// no textureStage. `surface` is a host pointer: the guest layer converts the game's address.
struct SurfaceDesc {
    std::uint32_t flags = 0;
    std::uint32_t height = 0;
    std::uint32_t width = 0;
    std::int32_t pitch = 0;
    std::uint32_t backBufferCount = 0;
    std::uint32_t mipMapCount = 0; // a display mode's refresh rate shares this field
    std::uint32_t alphaBitDepth = 0;
    std::uint8_t *surface = nullptr;
    ColorKey destOverlay;
    ColorKey destBlt;
    ColorKey srcOverlay;
    ColorKey srcBlt;
    PixelFormatDesc pixelFormat;
    SurfaceCaps caps;
    std::uint32_t textureStage = 0;
};

// The DDBLTFX fields Blt reads.
struct BltFx {
    std::uint32_t ddfx = 0;
    std::uint32_t rop = 0;
    std::uint32_t fillColor = 0;
    ColorKey destKey;
    ColorKey srcKey;
};

// The DDCAPS fields this module fills; every other field is zero.
struct DdCaps {
    std::uint32_t caps = 0;
    std::uint32_t caps2 = 0;
    std::uint32_t colorKeyCaps = 0;
    std::uint32_t fxCaps = 0;
    std::uint32_t paletteCaps = 0;
    std::uint32_t zBufferBitDepths = 0;
    std::uint32_t vidMemTotal = 0;
    std::uint32_t vidMemFree = 0;
    std::array<std::uint32_t, 8> rops{}; // one bit per raster operation index (the ROP's byte 2)
    SurfaceCaps surfaceCaps;
};

// PALETTEENTRY.
struct PaletteColor {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    std::uint8_t flags = 0;
};

// A 16-byte GUID as the game stores it, compared bytewise.
using Guid = std::array<std::uint8_t, 16>;

constexpr Guid makeGuid(std::uint32_t d1, std::uint16_t d2, std::uint16_t d3,
                        std::array<std::uint8_t, 8> d4) {
    return Guid{static_cast<std::uint8_t>(d1),
                static_cast<std::uint8_t>(d1 >> 8),
                static_cast<std::uint8_t>(d1 >> 16),
                static_cast<std::uint8_t>(d1 >> 24),
                static_cast<std::uint8_t>(d2),
                static_cast<std::uint8_t>(d2 >> 8),
                static_cast<std::uint8_t>(d3),
                static_cast<std::uint8_t>(d3 >> 8),
                d4[0],
                d4[1],
                d4[2],
                d4[3],
                d4[4],
                d4[5],
                d4[6],
                d4[7]};
}

// The interfaces the module serves (IIDs as in src/hle/com/).
constexpr Guid kIidUnknown = makeGuid(0x00000000, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46});
constexpr Guid kIidDirectDraw =
    makeGuid(0x6C14DB80, 0xA733, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60});
constexpr Guid kIidDirectDraw2 =
    makeGuid(0xB3A6F3E0, 0x2B43, 0x11CF, {0xA2, 0xDE, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56});
constexpr Guid kIidDirectDraw4 =
    makeGuid(0x9C59509A, 0x39BD, 0x11D1, {0x8C, 0x4A, 0x00, 0xC0, 0x4F, 0xD9, 0x30, 0xC5});
constexpr Guid kIidDirectDrawSurface =
    makeGuid(0x6C14DB81, 0xA733, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60});
constexpr Guid kIidDirectDrawSurface2 =
    makeGuid(0x57805885, 0x6EEC, 0x11CF, {0x94, 0x41, 0xA8, 0x23, 0x03, 0xC1, 0x0E, 0x27});
constexpr Guid kIidDirectDrawSurface3 =
    makeGuid(0xDA044E00, 0x69B2, 0x11D0, {0xA1, 0xD5, 0x00, 0xAA, 0x00, 0xB8, 0xDF, 0xBB});
constexpr Guid kIidDirectDrawSurface4 =
    makeGuid(0x0B2B8630, 0xAD35, 0x11D0, {0x8E, 0xA6, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B});
constexpr Guid kIidDirectDrawPalette =
    makeGuid(0x6C14DB84, 0xA733, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60});
constexpr Guid kIidDirectDrawClipper =
    makeGuid(0x6C14DB85, 0xA733, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60});

// The pixel format a DDPIXELFORMAT names, if the Gpu has it: 8-bit palettized, RGB 565, 555 and
// 1555 (as kArgb1555), XRGB and ARGB 8888, and a 16-bit z-buffer.
std::optional<platform::PixelFormat> formatOf(const PixelFormatDesc &desc);

// The DDPIXELFORMAT a surface of `format` reports.
PixelFormatDesc describe(platform::PixelFormat format);

// The display's format at a colour depth: 8 (palettized), 16 (RGB 565) or 32 (XRGB 8888).
std::optional<platform::PixelFormat> displayFormat(std::uint32_t bitsPerPixel);

// A 0xAARRGGBB colour as a raw pixel of `format` (for fills and clears).
std::uint32_t packColor(platform::PixelFormat format, std::uint32_t argb);

// The DirectDraw error a failed component call becomes.
HResult fromError(platform::Error error);

} // namespace sfcop::hle::ddraw4
