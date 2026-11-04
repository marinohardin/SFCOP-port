#include "hle/d3d3/Direct3D.h"
#include "hle/d3d3/Device.h"
#include "hle/d3d3/Viewport.h"
#include "hle/ddraw4/DirectDraw.h"
#include "hle/ddraw4/Surface.h"

namespace sfcop::hle::d3d3 {

namespace {

constexpr std::uint32_t kDefaultMaxTexture = 2048;

// What the Gpu rasterises, as D3DPRIMCAPS bits (d3dcaps.h).
PrimCaps primCaps() {
    PrimCaps caps;
    caps.miscCaps = 0x72;    // MASKZ, CULLNONE, CULLCW, CULLCCW
    caps.rasterCaps = 0x1B1; // DITHER, ZTEST, SUBPIXEL, FOGVERTEX, FOGTABLE
    caps.zCmpCaps = 0xFF;    // every comparison
    caps.alphaCmpCaps = 0xFF;
    caps.srcBlendCaps = 0x1FFF; // every blend factor, BOTHSRCALPHA and BOTHINVSRCALPHA
    caps.destBlendCaps = 0x1FFF;
    caps.shadeCaps = 0xC528A;            // flat and Gouraud colour, specular and alpha; fog
    caps.textureCaps = 0x8D;             // PERSPECTIVE, ALPHA, TRANSPARENCY, ALPHAPALETTE
    caps.textureFilterCaps = 0x0303033F; // point and linear, with point or linear mipmaps
    caps.textureBlendCaps = 0xCF;        // DECAL, MODULATE, DECALALPHA, MODULATEALPHA, COPY, ADD
    caps.textureAddressCaps = 0x17;      // WRAP, MIRROR, CLAMP, INDEPENDENTUV
    return caps;
}

} // namespace

Direct3D::Direct3D(ddraw4::DirectDraw &owner) : owner_(owner) {
    owner_.AdoptChild();
}

Direct3D::~Direct3D() {
    owner_.ChildGone();
}

DeviceInfo Direct3D::HalDevice() const {
    std::uint32_t maxTexture = kDefaultMaxTexture;
    platform::Result<platform::GpuCaps> caps = owner_.Device().GetCaps();
    if (caps.Ok() && caps.Value().maxTextureSize != 0) {
        maxTexture = caps.Value().maxTextureSize;
    }
    DeviceInfo info;
    info.guid = kIidHalDevice;
    info.description = "Hardware rasteriser";
    info.name = "Direct3D HAL";
    DeviceDesc &desc = info.hardware;
    desc.flags = 0x7FF; // every D3DDD_ field
    desc.colorModel = kColorRgb;
    // FLOATTLVERTEX, EXECUTESYSTEMMEMORY, TLVERTEXSYSTEMMEMORY, TEXTUREVIDEOMEMORY,
    // DRAWPRIMTLVERTEX, CANRENDERAFTERFLIP, DRAWPRIMITIVES2, HWRASTERIZATION
    desc.devCaps = 0x00082E51;
    desc.transformCaps = 0x1; // D3DTRANSFORMCAPS_CLIP
    desc.clipping = 1;
    desc.lightingModel = 0x1; // D3DLIGHTINGMODEL_RGB; no lights are offered
    desc.lineCaps = primCaps();
    desc.triCaps = primCaps();
    desc.renderBitDepth = ddraw4::kDdbd16 | ddraw4::kDdbd32;
    desc.zBufferBitDepth = ddraw4::kDdbd16;
    desc.maxVertexCount = 65535;
    desc.minTextureWidth = 1;
    desc.minTextureHeight = 1;
    desc.maxTextureWidth = maxTexture;
    desc.maxTextureHeight = maxTexture;
    desc.maxTextureRepeat = 8192;
    desc.maxTextureAspectRatio = maxTexture;
    desc.maxAnisotropy = 1;
    desc.fvfCaps = 0x00080002; // DONOTSTRIPELEMENTS, two coordinate sets
    // DISABLE, SELECTARG1, SELECTARG2, MODULATE, MODULATE2X, MODULATE4X, ADD, ADDSIGNED,
    // SUBTRACT, BLENDDIFFUSEALPHA, BLENDTEXTUREALPHA, BLENDFACTORALPHA, BLENDCURRENTALPHA
    desc.textureOpCaps = 0x0000BAFF;
    desc.maxTextureBlendStages = static_cast<std::uint16_t>(platform::kTextureStages);
    desc.maxSimultaneousTextures = static_cast<std::uint16_t>(platform::kTextureStages);
    return info;
}

HResult Direct3D::EnumDevices(std::vector<DeviceInfo> &out) const {
    out.assign(1, HalDevice());
    return kDdOk;
}

HResult Direct3D::FindDevice(const DeviceSearch &search, DeviceInfo &out) const {
    if ((search.flags & kFdsGuid) != 0 && search.guid != kIidHalDevice) {
        return ddraw4::kDdErrNotFound;
    }
    if ((search.flags & kFdsHardware) != 0 && !search.hardware) {
        return ddraw4::kDdErrNotFound;
    }
    if ((search.flags & kFdsColorModel) != 0 && search.colorModel != kColorRgb) {
        return ddraw4::kDdErrNotFound;
    }
    out = HalDevice();
    return kDdOk;
}

HResult Direct3D::CreateDevice(const Guid &device, ddraw4::Surface *target, Device **out) {
    if (out == nullptr) {
        return ddraw4::kDdErrInvalidParams;
    }
    *out = nullptr;
    if ((device != kIidHalDevice && device != kIidRgbDevice) || target == nullptr) {
        return ddraw4::kDdErrInvalidParams;
    }
    ddraw4::SurfaceCaps caps;
    (void)target->GetCaps(caps);
    if ((caps.caps & ddraw4::kDdscaps3dDevice) == 0 ||
        target->Format() == platform::PixelFormat::kIndexed8) {
        return ddraw4::kDdErrInvalidCaps;
    }
    *out = new Device(*this, *target); // the game's reference
    return kDdOk;
}

HResult Direct3D::CreateViewport(Viewport **out) {
    if (out == nullptr) {
        return ddraw4::kDdErrInvalidParams;
    }
    *out = new Viewport(*this); // the game's reference
    return kDdOk;
}

HResult Direct3D::EnumZBufferFormats(const Guid &device,
                                     std::vector<ddraw4::PixelFormatDesc> &out) const {
    out.clear();
    if (device != kIidHalDevice && device != kIidRgbDevice) {
        return ddraw4::kDdErrInvalidParams;
    }
    out.push_back(ddraw4::describe(platform::PixelFormat::kDepth16));
    return kDdOk;
}

} // namespace sfcop::hle::d3d3
