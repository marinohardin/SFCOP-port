// The Direct3D 3 constants and value types the module works with (DESIGN §6.1): return codes,
// render and texture stage states, vertex formats, and host-side copies of the structures the
// game passes by pointer. Numeric values are those of the public headers include/d3d.h,
// d3dtypes.h and d3dcaps.h of Wine 9.0. The guest shims (D3d3Guest.cpp) read and write the game's
// structures field by field into these types.
#pragma once

#include "hle/ddraw4/Ddraw4Defs.h"

#include <array>
#include <cstdint>

namespace sfcop::hle::d3d3 {

using ddraw4::ddHresult;
using ddraw4::Guid;
using ddraw4::HResult;
using ddraw4::kDdOk;
using ddraw4::makeGuid;

constexpr HResult kD3dErrInvalidCurrentViewport = ddHresult(735);
constexpr HResult kD3dErrInvalidPrimitiveType = ddHresult(736);
constexpr HResult kD3dErrInvalidVertexType = ddHresult(737);
constexpr HResult kD3dErrSceneInScene = ddHresult(760);
constexpr HResult kD3dErrSceneNotInScene = ddHresult(761);
constexpr HResult kD3dErrViewportDataNotSet = ddHresult(770);
constexpr HResult kD3dErrViewportHasNoDevice = ddHresult(774);
constexpr HResult kD3dErrNoCurrentViewport = ddHresult(775);
constexpr HResult kD3dErrInvalidVertexFormat = ddHresult(2048);
constexpr HResult kD3dErrZBufferNotPresent = ddHresult(2070);
constexpr HResult kD3dErrUnsupportedColorOperation = ddHresult(2073);
constexpr HResult kD3dErrUnsupportedColorArg = ddHresult(2074);
constexpr HResult kD3dErrUnsupportedAlphaOperation = ddHresult(2075);
constexpr HResult kD3dErrUnsupportedAlphaArg = ddHresult(2076);
constexpr HResult kD3dErrUnsupportedTextureFilter = ddHresult(2082);

// The one device this module offers: a hardware rasteriser on the Gpu.
constexpr Guid kIidHalDevice =
    makeGuid(0x84E63DE0, 0x46AA, 0x11CF, {0x81, 0x6F, 0x00, 0x00, 0xC0, 0x20, 0x15, 0x6E});
constexpr Guid kIidRgbDevice =
    makeGuid(0xA4665C60, 0x2673, 0x11CF, {0xA3, 0x1A, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56});
constexpr Guid kIidDirect3D3 =
    makeGuid(0xBB223240, 0xE72B, 0x11D0, {0xA9, 0xB4, 0x00, 0xAA, 0x00, 0xC0, 0x99, 0x3E});
constexpr Guid kIidDirect3DDevice3 =
    makeGuid(0xB0AB3B60, 0x33D7, 0x11D1, {0xA9, 0x81, 0x00, 0xC0, 0x4F, 0xD7, 0xB1, 0x74});
constexpr Guid kIidDirect3DTexture2 =
    makeGuid(0x93281502, 0x8CF8, 0x11D0, {0x89, 0xAB, 0x00, 0xA0, 0xC9, 0x05, 0x41, 0x29});
constexpr Guid kIidDirect3DViewport3 =
    makeGuid(0xB0AB3B61, 0x33D7, 0x11D1, {0xA9, 0x81, 0x00, 0xC0, 0x4F, 0xD7, 0xB1, 0x74});

// D3DRENDERSTATETYPE values the module acts on; every state up to kRenderStates is kept.
constexpr std::uint32_t kRenderStates = 256;
constexpr std::uint32_t kRsTextureHandle = 1;
constexpr std::uint32_t kRsTextureAddress = 3;
constexpr std::uint32_t kRsZEnable = 7;
constexpr std::uint32_t kRsFillMode = 8;
constexpr std::uint32_t kRsShadeMode = 9;
constexpr std::uint32_t kRsZWriteEnable = 14;
constexpr std::uint32_t kRsAlphaTestEnable = 15;
constexpr std::uint32_t kRsTextureMag = 17;
constexpr std::uint32_t kRsTextureMin = 18;
constexpr std::uint32_t kRsSrcBlend = 19;
constexpr std::uint32_t kRsDestBlend = 20;
constexpr std::uint32_t kRsTextureMapBlend = 21;
constexpr std::uint32_t kRsCullMode = 22;
constexpr std::uint32_t kRsZFunc = 23;
constexpr std::uint32_t kRsAlphaRef = 24;
constexpr std::uint32_t kRsAlphaFunc = 25;
constexpr std::uint32_t kRsAlphaBlendEnable = 27;
constexpr std::uint32_t kRsFogEnable = 28;
constexpr std::uint32_t kRsSpecularEnable = 29;
constexpr std::uint32_t kRsFogColor = 34;
constexpr std::uint32_t kRsFogTableMode = 35;
constexpr std::uint32_t kRsFogTableStart = 36;
constexpr std::uint32_t kRsFogTableEnd = 37;
constexpr std::uint32_t kRsFogTableDensity = 38;
constexpr std::uint32_t kRsColorKeyEnable = 41;
constexpr std::uint32_t kRsTextureAddressU = 44;
constexpr std::uint32_t kRsTextureAddressV = 45;
constexpr std::uint32_t kRsTextureFactor = 60;

// D3DTEXTURESTAGESTATETYPE values; every state up to kStageStates is kept, for 8 stages.
constexpr std::uint32_t kStages = 8;
constexpr std::uint32_t kStageStates = 32;
constexpr std::uint32_t kTssColorOp = 1;
constexpr std::uint32_t kTssColorArg1 = 2;
constexpr std::uint32_t kTssColorArg2 = 3;
constexpr std::uint32_t kTssAlphaOp = 4;
constexpr std::uint32_t kTssAlphaArg1 = 5;
constexpr std::uint32_t kTssAlphaArg2 = 6;
constexpr std::uint32_t kTssTexCoordIndex = 11;
constexpr std::uint32_t kTssAddress = 12;
constexpr std::uint32_t kTssAddressU = 13;
constexpr std::uint32_t kTssAddressV = 14;
constexpr std::uint32_t kTssMagFilter = 16;
constexpr std::uint32_t kTssMinFilter = 17;
constexpr std::uint32_t kTssMipFilter = 18;

// D3DTEXTUREOP and D3DTA_*.
constexpr std::uint32_t kTopDisable = 1;
constexpr std::uint32_t kTopSelectArg1 = 2;
constexpr std::uint32_t kTopSelectArg2 = 3;
constexpr std::uint32_t kTopModulate = 4;
constexpr std::uint32_t kTopModulate2x = 5;
constexpr std::uint32_t kTopModulate4x = 6;
constexpr std::uint32_t kTopAdd = 7;
constexpr std::uint32_t kTopAddSigned = 8;
constexpr std::uint32_t kTopSubtract = 10;
constexpr std::uint32_t kTopBlendDiffuseAlpha = 12;
constexpr std::uint32_t kTopBlendTextureAlpha = 13;
constexpr std::uint32_t kTopBlendFactorAlpha = 14;
constexpr std::uint32_t kTopBlendCurrentAlpha = 16;
constexpr std::uint32_t kTaDiffuse = 0;
constexpr std::uint32_t kTaCurrent = 1;
constexpr std::uint32_t kTaTexture = 2;
constexpr std::uint32_t kTaTFactor = 3;
constexpr std::uint32_t kTaSpecular = 4;
constexpr std::uint32_t kTaSelectMask = 0x0F;

// D3DTEXTUREBLEND (the legacy TEXTUREMAPBLEND state) and D3DTEXTUREFILTER.
constexpr std::uint32_t kTblendDecal = 1;
constexpr std::uint32_t kTblendModulate = 2;
constexpr std::uint32_t kTblendDecalAlpha = 3;
constexpr std::uint32_t kTblendModulateAlpha = 4;
constexpr std::uint32_t kTblendCopy = 7;
constexpr std::uint32_t kTblendAdd = 8;

// D3DPRIMITIVETYPE.
constexpr std::uint32_t kPtPointList = 1;
constexpr std::uint32_t kPtTriangleFan = 6;

// The flexible vertex format bits.
constexpr std::uint32_t kFvfPositionMask = 0x00E;
constexpr std::uint32_t kFvfXyz = 0x002;
constexpr std::uint32_t kFvfXyzRhw = 0x004;
constexpr std::uint32_t kFvfNormal = 0x010;
constexpr std::uint32_t kFvfReserved1 = 0x020;
constexpr std::uint32_t kFvfDiffuse = 0x040;
constexpr std::uint32_t kFvfSpecular = 0x080;
constexpr std::uint32_t kFvfTexCountShift = 8;
constexpr std::uint32_t kFvfTexCountMask = 0xF00;
constexpr std::uint32_t kFvfTlVertex = 0x1C4; // XYZRHW, DIFFUSE, SPECULAR, TEX1

// D3DCLEAR_*.
constexpr std::uint32_t kClearTarget = 0x1;
constexpr std::uint32_t kClearZBuffer = 0x2;
constexpr std::uint32_t kClearStencil = 0x4;

// D3DTRANSFORMSTATETYPE.
constexpr std::uint32_t kTransformWorld = 1;
constexpr std::uint32_t kTransformView = 2;
constexpr std::uint32_t kTransformProjection = 3;

// D3DFINDDEVICESEARCH.dwFlags.
constexpr std::uint32_t kFdsColorModel = 0x1;
constexpr std::uint32_t kFdsGuid = 0x2;
constexpr std::uint32_t kFdsHardware = 0x4;

constexpr std::uint32_t kColorRgb = 2; // D3DCOLOR_RGB

// D3DMATRIX, row-major as the game stores it (_11 .. _44).
using Matrix = std::array<float, 16>;

constexpr Matrix kIdentity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

// D3DRECT: x1, y1 inclusive; x2, y2 exclusive.
struct D3dRect {
    std::int32_t x1 = 0;
    std::int32_t y1 = 0;
    std::int32_t x2 = 0;
    std::int32_t y2 = 0;
};

// D3DVIEWPORT2 without its dwSize. A D3DVIEWPORT is converted to this form (Viewport.h).
struct ViewportDesc {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    float clipX = -1.0f;
    float clipY = 1.0f;
    float clipWidth = 2.0f;
    float clipHeight = 2.0f;
    float minZ = 0.0f;
    float maxZ = 1.0f;
};

// D3DPRIMCAPS without its dwSize.
struct PrimCaps {
    std::uint32_t miscCaps = 0;
    std::uint32_t rasterCaps = 0;
    std::uint32_t zCmpCaps = 0;
    std::uint32_t srcBlendCaps = 0;
    std::uint32_t destBlendCaps = 0;
    std::uint32_t alphaCmpCaps = 0;
    std::uint32_t shadeCaps = 0;
    std::uint32_t textureCaps = 0;
    std::uint32_t textureFilterCaps = 0;
    std::uint32_t textureBlendCaps = 0;
    std::uint32_t textureAddressCaps = 0;
    std::uint32_t stippleWidth = 0;
    std::uint32_t stippleHeight = 0;
};

// D3DDEVICEDESC (the DirectX 6 form) without its dwSize; the guard band and stipple fields,
// always zero here, are left out.
struct DeviceDesc {
    std::uint32_t flags = 0;
    std::uint32_t colorModel = 0;
    std::uint32_t devCaps = 0;
    std::uint32_t transformCaps = 0;
    std::uint32_t clipping = 0;
    std::uint32_t lightingCaps = 0;
    std::uint32_t lightingModel = 0;
    std::uint32_t maxLights = 0;
    PrimCaps lineCaps;
    PrimCaps triCaps;
    std::uint32_t renderBitDepth = 0;
    std::uint32_t zBufferBitDepth = 0;
    std::uint32_t maxBufferSize = 0;
    std::uint32_t maxVertexCount = 0;
    std::uint32_t minTextureWidth = 0;
    std::uint32_t minTextureHeight = 0;
    std::uint32_t maxTextureWidth = 0;
    std::uint32_t maxTextureHeight = 0;
    std::uint32_t maxTextureRepeat = 0;
    std::uint32_t maxTextureAspectRatio = 0;
    std::uint32_t maxAnisotropy = 0;
    std::uint32_t stencilCaps = 0;
    std::uint32_t fvfCaps = 0;
    std::uint32_t textureOpCaps = 0;
    std::uint16_t maxTextureBlendStages = 0;
    std::uint16_t maxSimultaneousTextures = 0;
};

// One device as EnumDevices and FindDevice describe it.
struct DeviceInfo {
    Guid guid{};
    const char *description = "";
    const char *name = "";
    DeviceDesc hardware;
    DeviceDesc software;
};

// The D3DFINDDEVICESEARCH fields FindDevice looks at.
struct DeviceSearch {
    std::uint32_t flags = 0;
    bool hardware = false;
    std::uint32_t colorModel = 0;
    Guid guid{};
};

} // namespace sfcop::hle::d3d3
