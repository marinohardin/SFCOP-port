// The guest shims of the Direct3D module (src/hle/README.md, "Implementing a module"): the methods
// of IDirect3D3, IDirect3DDevice3, IDirect3DTexture2 and IDirect3DViewport3, one shim per row of
// src/hle/surface.tsv; shim<Method, Bytes> purges the row's stack bytes.
//
// Direct3D has no DLL export of its own, so sfcop_hle_d3d3[] is empty. The game reaches it
// through QueryInterface on the DirectDraw objects: IDirect3D3 from a DirectDraw object and
// IDirect3DTexture2 from a surface. The DirectDraw shims hand an interface they do not serve to
// the hook this file installs (GuestViews::SetQueryHook), which makes the Direct3D object, one per
// DirectDraw object, or the texture, one per surface.
//
// TEMPORARY: until the build compiles *Guest.* files only with the lift, this file is also
// compiled into sfcop_src, where the lift's headers are not on the include path; it is then
// empty.
#if __has_include("win32hle.h")

#include "hle/d3d3/Device.h"
#include "hle/d3d3/Direct3D.h"
#include "hle/d3d3/Texture.h"
#include "hle/d3d3/Viewport.h"
#include "hle/d3d3/Views.h"
#include "hle/ddraw4/DirectDraw.h"
#include "hle/ddraw4/GuestAccess.h"
#include "hle/ddraw4/GuestViews.h"
#include "hle/ddraw4/Surface.h"

#include <cstring>
#include <map>
#include <vector>

namespace sfcop::hle::d3d3 {

namespace {

using namespace ddraw4::guest;
using ddraw4::ComObject;
using ddraw4::GuestViews;
using ddraw4::kDdErrInvalidObject;
using ddraw4::kDdErrInvalidParams;
using ddraw4::kDdErrOutOfMemory;
using ddraw4::kDdErrUnsupported;
using GI = ddraw4::GuestInterface;

GuestViews &views() {
    return GuestViews::Instance();
}

template <std::uint32_t (*Method)(), std::uint32_t kStackBytes> void shim() {
    const std::uint32_t result = Method();
    RET(result, kStackBytes / 4);
}

// D3DENUMRET_CANCEL.
constexpr std::uint32_t kEnumCancel = 0;

Direct3D *direct3D(std::uint32_t address) {
    return static_cast<Direct3D *>(views().Host(address, GI::kDirect3D3, GI::kDirect3D3));
}

Device *device(std::uint32_t address) {
    return static_cast<Device *>(views().Host(address, GI::kDevice3, GI::kDevice3));
}

Viewport *viewport(std::uint32_t address) {
    return static_cast<Viewport *>(views().Host(address, GI::kViewport3, GI::kViewport3));
}

Texture *texture(std::uint32_t address) {
    return static_cast<Texture *>(views().Host(address, GI::kTexture2, GI::kTexture2));
}

ddraw4::Surface *surface(std::uint32_t address) {
    return static_cast<ddraw4::Surface *>(views().Host(address, GI::kSurface3, GI::kSurface4));
}

template <class T> bool optionalArg(std::uint32_t address, T *(*find)(std::uint32_t), T *&out) {
    out = address == 0 ? nullptr : find(address);
    return address == 0 || out != nullptr;
}

HResult handOut(std::uint32_t out, ComObject *object, GI iface) {
    const std::uint32_t view = views().View(object, iface);
    if (view == 0) {
        object->Release();
        return kDdErrOutOfMemory;
    }
    write32(out, view);
    return kDdOk;
}

// The game's out-parameter, cleared; 0 if it passed none.
std::uint32_t clearedOut(std::uint32_t address) {
    if (address != 0) {
        write32(address, 0);
    }
    return address;
}

// ---- structures ----

// D3DPRIMCAPS: dwSize, then the 13 fields of PrimCaps (56 bytes).
void writePrimCaps(std::uint32_t address, const PrimCaps &caps) {
    const std::uint32_t fields[] = {56,
                                    caps.miscCaps,
                                    caps.rasterCaps,
                                    caps.zCmpCaps,
                                    caps.srcBlendCaps,
                                    caps.destBlendCaps,
                                    caps.alphaCmpCaps,
                                    caps.shadeCaps,
                                    caps.textureCaps,
                                    caps.textureFilterCaps,
                                    caps.textureBlendCaps,
                                    caps.textureAddressCaps,
                                    caps.stippleWidth,
                                    caps.stippleHeight};
    for (std::uint32_t i = 0; i < 14; ++i) {
        write32(address + 4 * i, fields[i]);
    }
}

// D3DDEVICEDESC in its DirectX 6 form is 252 bytes; earlier forms are prefixes of it. The game's
// dwSize says how much to write.
constexpr std::uint32_t kDeviceDescSize = 252;

void writeDeviceDesc(std::uint32_t address, const DeviceDesc &desc, std::uint32_t size) {
    std::uint8_t full[kDeviceDescSize] = {};
    const auto put = [&](std::uint32_t offset, std::uint32_t value) {
        std::memcpy(full + offset, &value, 4);
    };
    put(0, size);
    put(4, desc.flags);
    put(8, desc.colorModel);
    put(12, desc.devCaps);
    put(16, 8); // D3DTRANSFORMCAPS
    put(20, desc.transformCaps);
    put(24, desc.clipping);
    put(28, 16); // D3DLIGHTINGCAPS
    put(32, desc.lightingCaps);
    put(36, desc.lightingModel);
    put(40, desc.maxLights);
    put(156, desc.renderBitDepth);
    put(160, desc.zBufferBitDepth);
    put(164, desc.maxBufferSize);
    put(168, desc.maxVertexCount);
    put(172, desc.minTextureWidth);
    put(176, desc.minTextureHeight);
    put(180, desc.maxTextureWidth);
    put(184, desc.maxTextureHeight);
    put(204, desc.maxTextureRepeat);
    put(208, desc.maxTextureAspectRatio);
    put(212, desc.maxAnisotropy);
    put(236, desc.stencilCaps);
    put(240, desc.fvfCaps);
    put(244, desc.textureOpCaps);
    put(248, desc.maxTextureBlendStages | (std::uint32_t{desc.maxSimultaneousTextures} << 16));
    copyOut(address, full, std::min(size, kDeviceDescSize));
    if (size >= 44 + 56) {
        writePrimCaps(address + 44, desc.lineCaps);
    }
    if (size >= 100 + 56) {
        writePrimCaps(address + 100, desc.triCaps);
    }
}

bool isDeviceDescSize(std::uint32_t size) {
    return size >= 4 * 4 && size <= kDeviceDescSize;
}

// D3DVIEWPORT2 and D3DVIEWPORT: dwSize, dwX, dwY, dwWidth, dwHeight, four floats, dvMinZ, dvMaxZ
// (44 bytes each).
constexpr std::uint32_t kViewportSize = 44;

ViewportDesc readViewport2(std::uint32_t address) {
    ViewportDesc desc;
    desc.x = read32(address + 4);
    desc.y = read32(address + 8);
    desc.width = read32(address + 12);
    desc.height = read32(address + 16);
    desc.clipX = readFloat(address + 20);
    desc.clipY = readFloat(address + 24);
    desc.clipWidth = readFloat(address + 28);
    desc.clipHeight = readFloat(address + 32);
    desc.minZ = readFloat(address + 36);
    desc.maxZ = readFloat(address + 40);
    return desc;
}

void writeViewport2(std::uint32_t address, const ViewportDesc &desc) {
    write32(address, kViewportSize);
    write32(address + 4, desc.x);
    write32(address + 8, desc.y);
    write32(address + 12, desc.width);
    write32(address + 16, desc.height);
    writeFloat(address + 20, desc.clipX);
    writeFloat(address + 24, desc.clipY);
    writeFloat(address + 28, desc.clipWidth);
    writeFloat(address + 32, desc.clipHeight);
    writeFloat(address + 36, desc.minZ);
    writeFloat(address + 40, desc.maxZ);
}

ViewportV1 readViewport1(std::uint32_t address) {
    ViewportV1 desc;
    desc.x = read32(address + 4);
    desc.y = read32(address + 8);
    desc.width = read32(address + 12);
    desc.height = read32(address + 16);
    desc.scaleX = readFloat(address + 20);
    desc.scaleY = readFloat(address + 24);
    desc.maxX = readFloat(address + 28);
    desc.maxY = readFloat(address + 32);
    desc.minZ = readFloat(address + 36);
    desc.maxZ = readFloat(address + 40);
    return desc;
}

void writeViewport1(std::uint32_t address, const ViewportV1 &desc) {
    write32(address, kViewportSize);
    write32(address + 4, desc.x);
    write32(address + 8, desc.y);
    write32(address + 12, desc.width);
    write32(address + 16, desc.height);
    writeFloat(address + 20, desc.scaleX);
    writeFloat(address + 24, desc.scaleY);
    writeFloat(address + 28, desc.maxX);
    writeFloat(address + 32, desc.maxY);
    writeFloat(address + 36, desc.minZ);
    writeFloat(address + 40, desc.maxZ);
}

// D3DRECT: x1, y1, x2, y2.
std::vector<D3dRect> readRects(std::uint32_t address, std::uint32_t count) {
    std::vector<D3dRect> rects(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t at = address + 16 * i;
        rects[i] = D3dRect{
            static_cast<std::int32_t>(read32(at)), static_cast<std::int32_t>(read32(at + 4)),
            static_cast<std::int32_t>(read32(at + 8)), static_cast<std::int32_t>(read32(at + 12))};
    }
    return rects;
}

// `count` 16-bit indices copied from guest memory (the guest's address need not be aligned for the
// host), or std::nullopt for a count no draw can have.
std::optional<std::vector<std::uint16_t>> readIndices(std::uint32_t address, std::uint32_t count) {
    constexpr std::uint32_t kMaxIndices = 1U << 20;
    if (count > kMaxIndices) {
        return std::nullopt;
    }
    std::vector<std::uint16_t> indices(count);
    if (count != 0) {
        std::memcpy(indices.data(), hle_ptr(address), 2U * count);
    }
    return indices;
}

// D3DMATRIX: 16 floats, row by row.
Matrix readMatrix(std::uint32_t address) {
    Matrix matrix{};
    for (std::uint32_t i = 0; i < 16; ++i) {
        matrix[i] = readFloat(address + 4 * i);
    }
    return matrix;
}

void writeMatrix(std::uint32_t address, const Matrix &matrix) {
    for (std::uint32_t i = 0; i < 16; ++i) {
        writeFloat(address + 4 * i, matrix[i]);
    }
}

// ---- IUnknown ----

std::uint32_t queryInterface() { // (this, riid, ppv)
    const auto view = views().Find(A32(0));
    const std::uint32_t iid = A32(1);
    const std::uint32_t out = A32(2);
    if (!view) {
        return kDdErrInvalidObject;
    }
    if (iid == 0 || out == 0) {
        return kDdErrInvalidParams;
    }
    write32(out, 0);
    const Guid wanted = readGuid(iid);
    const std::uint32_t result = views().Query(*view->host, view->iface, wanted);
    if (result == 0) {
        return ddraw4::kENoInterface;
    }
    write32(out, result);
    return kDdOk;
}

std::uint32_t addRef() { // (this)
    const auto view = views().Find(A32(0));
    return view ? view->host->AddRef() : 0;
}

std::uint32_t release() { // (this)
    const auto view = views().Find(A32(0));
    return view ? view->host->Release() : 0;
}

// ---- IDirect3D3 ----

std::uint32_t d3dEnumDevices() { // (this, callback, context)
    Direct3D *self = direct3D(A32(0));
    const std::uint32_t callback = A32(1);
    const std::uint32_t context = A32(2);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (callback == 0) {
        return kDdErrInvalidParams;
    }
    std::vector<DeviceInfo> devices;
    const HResult result = self->EnumDevices(devices);
    if (result != kDdOk) {
        return result;
    }
    for (const DeviceInfo &info : devices) {
        Scratch scratch;
        const std::uint32_t guid = scratch.Take(16);
        writeGuid(guid, info.guid);
        const std::uint32_t hardware = scratch.Take(kDeviceDescSize);
        const std::uint32_t software = scratch.Take(kDeviceDescSize);
        writeDeviceDesc(hardware, info.hardware, kDeviceDescSize);
        writeDeviceDesc(software, info.software, kDeviceDescSize);
        if (callGuest(callback, {guid, scratch.Text(info.description), scratch.Text(info.name),
                                 hardware, software, context}) == kEnumCancel) {
            break;
        }
    }
    return kDdOk;
}

std::uint32_t d3dUnsupportedCreate() { // CreateLight, CreateMaterial: (this, lplpObject, outer)
    clearedOut(A32(1));
    return direct3D(A32(0)) == nullptr ? kDdErrInvalidObject : kDdErrUnsupported;
}

std::uint32_t d3dCreateViewport() { // (this, lplpViewport, outer)
    Direct3D *self = direct3D(A32(0));
    const std::uint32_t out = clearedOut(A32(1));
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    if (A32(2) != 0) {
        return ddraw4::kClassENoAggregation;
    }
    Viewport *created = nullptr;
    const HResult result = self->CreateViewport(&created);
    return result != kDdOk ? result : handOut(out, created, GI::kViewport3);
}

// D3DFINDDEVICESEARCH: dwSize, dwFlags, bHardware, dcmColorModel, guid, dwCaps, dpcPrimCaps.
// D3DFINDDEVICERESULT: dwSize, guid, ddHwDesc, ddSwDesc, the two descriptions sharing what the
// dwSize leaves after the GUID.
std::uint32_t d3dFindDevice() { // (this, search, result)
    Direct3D *self = direct3D(A32(0));
    const std::uint32_t searchAddress = A32(1);
    const std::uint32_t out = A32(2);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (searchAddress == 0 || out == 0 || read32(out) < 20) {
        return kDdErrInvalidParams;
    }
    DeviceSearch search;
    search.flags = read32(searchAddress + 4);
    search.hardware = read32(searchAddress + 8) != 0;
    search.colorModel = read32(searchAddress + 12);
    search.guid = readGuid(searchAddress + 16);
    DeviceInfo found;
    const HResult result = self->FindDevice(search, found);
    if (result != kDdOk) {
        return result;
    }
    const std::uint32_t descSize = std::min((read32(out) - 20) / 2, kDeviceDescSize);
    writeGuid(out + 4, found.guid);
    if (isDeviceDescSize(descSize)) {
        writeDeviceDesc(out + 20, found.hardware, descSize);
        writeDeviceDesc(out + 20 + descSize, found.software, descSize);
    }
    return kDdOk;
}

std::uint32_t d3dCreateDevice() { // (this, guid, surface, lplpDevice, outer)
    Direct3D *self = direct3D(A32(0));
    const std::uint32_t guid = A32(1);
    ddraw4::Surface *target = surface(A32(2));
    const std::uint32_t out = clearedOut(A32(3));
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (guid == 0 || target == nullptr || out == 0) {
        return kDdErrInvalidParams;
    }
    if (A32(4) != 0) {
        return ddraw4::kClassENoAggregation;
    }
    Device *created = nullptr;
    const HResult result = self->CreateDevice(readGuid(guid), target, &created);
    return result != kDdOk ? result : handOut(out, created, GI::kDevice3);
}

std::uint32_t d3dCreateVertexBuffer() { // (this, desc, lplpBuffer, flags, outer): not served
    clearedOut(A32(2));
    return direct3D(A32(0)) == nullptr ? kDdErrInvalidObject : kDdErrUnsupported;
}

std::uint32_t enumPixelFormats(std::uint32_t callback, std::uint32_t context,
                               const std::vector<ddraw4::PixelFormatDesc> &formats) {
    for (const ddraw4::PixelFormatDesc &format : formats) {
        Scratch scratch;
        const std::uint32_t address = scratch.Take(kPixelFormatSize);
        writePixelFormat(address, format);
        if (callGuest(callback, {address, context}) == kEnumCancel) {
            break;
        }
    }
    return kDdOk;
}

std::uint32_t d3dEnumZBufferFormats() { // (this, guid, callback, context)
    Direct3D *self = direct3D(A32(0));
    const std::uint32_t guid = A32(1);
    const std::uint32_t callback = A32(2);
    const std::uint32_t context = A32(3);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (guid == 0 || callback == 0) {
        return kDdErrInvalidParams;
    }
    std::vector<ddraw4::PixelFormatDesc> formats;
    const HResult result = self->EnumZBufferFormats(readGuid(guid), formats);
    return result != kDdOk ? result : enumPixelFormats(callback, context, formats);
}

std::uint32_t d3dEvictManagedTextures() { // (this)
    Direct3D *self = direct3D(A32(0));
    return self == nullptr ? kDdErrInvalidObject : self->EvictManagedTextures();
}

// ---- IDirect3DDevice3 ----

std::uint32_t devGetCaps() { // (this, hardwareDesc, softwareDesc)
    Device *self = device(A32(0));
    const std::uint32_t hardware = A32(1);
    const std::uint32_t software = A32(2);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (hardware == 0 || software == 0 || !isDeviceDescSize(read32(hardware)) ||
        !isDeviceDescSize(read32(software))) {
        return kDdErrInvalidParams;
    }
    DeviceDesc hardwareDesc;
    DeviceDesc softwareDesc;
    const HResult result = self->GetCaps(&hardwareDesc, &softwareDesc);
    if (result == kDdOk) {
        writeDeviceDesc(hardware, hardwareDesc, read32(hardware));
        writeDeviceDesc(software, softwareDesc, read32(software));
    }
    return result;
}

// D3DSTATS: dwSize and five counts, all zero (the module counts nothing).
std::uint32_t devGetStats() { // (this, stats)
    const std::uint32_t out = A32(1);
    if (device(A32(0)) == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0 || read32(out) < 24) {
        return kDdErrInvalidParams;
    }
    zero(out + 4, 20);
    return kDdOk;
}

std::uint32_t devAddViewport() { // (this, viewport)
    Device *self = device(A32(0));
    Viewport *added = viewport(A32(1));
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    return added == nullptr ? kDdErrInvalidParams : self->AddViewport(added);
}

std::uint32_t devDeleteViewport() { // (this, viewport)
    Device *self = device(A32(0));
    Viewport *deleted = viewport(A32(1));
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    return deleted == nullptr ? kDdErrInvalidParams : self->DeleteViewport(deleted);
}

std::uint32_t devNextViewport() { // (this, viewport, lplpViewport, flags)
    Device *self = device(A32(0));
    Viewport *from = nullptr;
    const std::uint32_t out = clearedOut(A32(2));
    if (self == nullptr || !optionalArg(A32(1), viewport, from)) {
        return kDdErrInvalidObject;
    }
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    Viewport *found = nullptr;
    const HResult result = self->NextViewport(from, A32(3), &found);
    if (result != kDdOk) {
        return result;
    }
    // Not AddRef'd, as Direct3D documents for NextViewport.
    const std::uint32_t view = views().View(found, GI::kViewport3);
    if (view == 0) {
        return kDdErrOutOfMemory;
    }
    write32(out, view);
    return kDdOk;
}

std::uint32_t devEnumTextureFormats() { // (this, callback, context)
    Device *self = device(A32(0));
    const std::uint32_t callback = A32(1);
    const std::uint32_t context = A32(2);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (callback == 0) {
        return kDdErrInvalidParams;
    }
    std::vector<ddraw4::PixelFormatDesc> formats;
    const HResult result = self->EnumTextureFormats(formats);
    return result != kDdOk ? result : enumPixelFormats(callback, context, formats);
}

std::uint32_t devBeginScene() { // (this)
    Device *self = device(A32(0));
    return self == nullptr ? kDdErrInvalidObject : self->BeginScene();
}

std::uint32_t devEndScene() { // (this)
    Device *self = device(A32(0));
    return self == nullptr ? kDdErrInvalidObject : self->EndScene();
}

std::uint32_t devGetDirect3D() { // (this, lplpDirect3D)
    Device *self = device(A32(0));
    const std::uint32_t out = clearedOut(A32(1));
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    Direct3D &owner = self->Owner();
    owner.AddRef();
    return handOut(out, &owner, GI::kDirect3D3);
}

std::uint32_t devSetCurrentViewport() { // (this, viewport)
    Device *self = device(A32(0));
    Viewport *current = viewport(A32(1));
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    return current == nullptr ? kDdErrInvalidParams : self->SetCurrentViewport(current);
}

std::uint32_t devGetCurrentViewport() { // (this, lplpViewport)
    Device *self = device(A32(0));
    const std::uint32_t out = clearedOut(A32(1));
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    Viewport *current = nullptr;
    const HResult result = self->GetCurrentViewport(&current);
    return result != kDdOk ? result : handOut(out, current, GI::kViewport3);
}

std::uint32_t devSetRenderTarget() { // (this, surface, flags)
    Device *self = device(A32(0));
    ddraw4::Surface *target = surface(A32(1));
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    return target == nullptr ? kDdErrInvalidParams : self->SetRenderTarget(target, A32(2));
}

std::uint32_t devGetRenderTarget() { // (this, lplpSurface)
    Device *self = device(A32(0));
    const std::uint32_t out = clearedOut(A32(1));
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    ddraw4::Surface *target = nullptr;
    const HResult result = self->GetRenderTarget(&target);
    return result != kDdOk ? result : handOut(out, target, GI::kSurface4);
}

// Begin, BeginIndexed, Vertex, Index and End: Direct3D 2's vertex-at-a-time drawing; not served.
std::uint32_t devUnsupported() {
    return device(A32(0)) == nullptr ? kDdErrInvalidObject : kDdErrUnsupported;
}

std::uint32_t devGetRenderState() { // (this, state, value)
    Device *self = device(A32(0));
    const std::uint32_t out = A32(2);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    std::uint32_t value = 0;
    const HResult result = self->GetRenderState(A32(1), value);
    if (result == kDdOk) {
        write32(out, value);
    }
    return result;
}

std::uint32_t devSetRenderState() { // (this, state, value)
    Device *self = device(A32(0));
    return self == nullptr ? kDdErrInvalidObject : self->SetRenderState(A32(1), A32(2));
}

std::uint32_t devGetLightState() { // (this, state, value)
    Device *self = device(A32(0));
    const std::uint32_t out = A32(2);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    std::uint32_t value = 0;
    const HResult result = self->GetLightState(A32(1), value);
    if (result == kDdOk) {
        write32(out, value);
    }
    return result;
}

std::uint32_t devSetLightState() { // (this, state, value)
    Device *self = device(A32(0));
    return self == nullptr ? kDdErrInvalidObject : self->SetLightState(A32(1), A32(2));
}

std::uint32_t devSetTransform() { // (this, type, matrix)
    Device *self = device(A32(0));
    const std::uint32_t matrix = A32(2);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    return matrix == 0 ? kDdErrInvalidParams : self->SetTransform(A32(1), readMatrix(matrix));
}

std::uint32_t devGetTransform() { // (this, type, matrix)
    Device *self = device(A32(0));
    const std::uint32_t out = A32(2);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    Matrix matrix{};
    const HResult result = self->GetTransform(A32(1), matrix);
    if (result == kDdOk) {
        writeMatrix(out, matrix);
    }
    return result;
}

std::uint32_t devMultiplyTransform() { // (this, type, matrix)
    Device *self = device(A32(0));
    const std::uint32_t matrix = A32(2);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    return matrix == 0 ? kDdErrInvalidParams : self->MultiplyTransform(A32(1), readMatrix(matrix));
}

std::uint32_t devDrawPrimitive() { // (this, type, fvf, vertices, count, flags)
    Device *self = device(A32(0));
    const std::uint32_t vertices = A32(3);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (vertices == 0) {
        return kDdErrInvalidParams;
    }
    return self->DrawPrimitive(A32(1), A32(2), hle_ptr(vertices), A32(4), A32(5));
}

// (this, type, fvf, vertices, vertexCount, indices, indexCount, flags)
std::uint32_t devDrawIndexedPrimitive() {
    Device *self = device(A32(0));
    const std::uint32_t vertices = A32(3);
    const std::uint32_t indices = A32(5);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (vertices == 0 || indices == 0) {
        return kDdErrInvalidParams;
    }
    const auto copied = readIndices(indices, A32(6));
    if (!copied) {
        return kDdErrInvalidParams;
    }
    return self->DrawIndexedPrimitive(A32(1), A32(2), hle_ptr(vertices), A32(4), copied->data(),
                                      A32(6), A32(7));
}

std::uint32_t devSetClipStatus() { // (this, status): kept nowhere, nothing is clipped by it
    return device(A32(0)) == nullptr ? kDdErrInvalidObject : kDdOk;
}

// D3DCLIPSTATUS: dwFlags, dwStatus and six extents (32 bytes), all zero.
std::uint32_t devGetClipStatus() { // (this, status)
    const std::uint32_t out = A32(1);
    if (device(A32(0)) == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    zero(out, 32);
    return kDdOk;
}

// D3DDRAWPRIMITIVESTRIDEDDATA: {lpvData, dwStride} for the position, normal, diffuse, specular
// and eight coordinate sets. The vertices are gathered into the flexible format's own layout
// (two floats per coordinate set) and drawn as DrawPrimitive draws them.
std::optional<std::vector<std::uint8_t>> gather(std::uint32_t fvf, std::uint32_t strided,
                                                std::uint32_t count) {
    const std::uint32_t position = fvf & kFvfPositionMask;
    if (strided == 0 || (position != kFvfXyz && position != kFvfXyzRhw)) {
        return std::nullopt;
    }
    struct Part {
        std::uint32_t entry;
        std::uint32_t bytes;
    };
    std::vector<Part> parts{{0, position == kFvfXyzRhw ? 16u : 12u}};
    if ((fvf & kFvfNormal) != 0) {
        parts.push_back({8, 12});
    }
    if ((fvf & kFvfReserved1) != 0) {
        parts.push_back({~0u, 4});
    }
    if ((fvf & kFvfDiffuse) != 0) {
        parts.push_back({16, 4});
    }
    if ((fvf & kFvfSpecular) != 0) {
        parts.push_back({24, 4});
    }
    const std::uint32_t sets = (fvf & kFvfTexCountMask) >> kFvfTexCountShift;
    for (std::uint32_t set = 0; set < sets && set < 8; ++set) {
        parts.push_back({32 + 8 * set, 8});
    }
    std::uint32_t stride = 0;
    for (const Part &part : parts) {
        stride += part.bytes;
    }
    std::vector<std::uint8_t> out(static_cast<std::size_t>(stride) * count);
    std::uint32_t offset = 0;
    for (const Part &part : parts) {
        if (part.entry != ~0u) {
            const std::uint32_t data = read32(strided + part.entry);
            const std::uint32_t step = read32(strided + part.entry + 4);
            for (std::uint32_t i = 0; i < count; ++i) {
                copyIn(out.data() + static_cast<std::size_t>(i) * stride + offset, data + i * step,
                       part.bytes);
            }
        }
        offset += part.bytes;
    }
    return out;
}

std::uint32_t devDrawPrimitiveStrided() { // (this, type, fvf, strided, count, flags)
    Device *self = device(A32(0));
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    const auto vertices = gather(A32(2), A32(3), A32(4));
    if (!vertices) {
        return kDdErrInvalidParams;
    }
    return self->DrawPrimitive(A32(1), A32(2), vertices->data(), A32(4), A32(5));
}

// (this, type, fvf, strided, vertexCount, indices, indexCount, flags)
std::uint32_t devDrawIndexedPrimitiveStrided() {
    Device *self = device(A32(0));
    const std::uint32_t indices = A32(5);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    const auto vertices = gather(A32(2), A32(3), A32(4));
    if (!vertices || indices == 0) {
        return kDdErrInvalidParams;
    }
    const auto copied = readIndices(indices, A32(6));
    if (!copied) {
        return kDdErrInvalidParams;
    }
    return self->DrawIndexedPrimitive(A32(1), A32(2), vertices->data(), A32(4), copied->data(),
                                      A32(6), A32(7));
}

// (this, centers, radii, count, flags, results): every sphere visible (status 0).
std::uint32_t devComputeSphereVisibility() {
    const std::uint32_t count = A32(3);
    const std::uint32_t results = A32(5);
    if (device(A32(0)) == nullptr) {
        return kDdErrInvalidObject;
    }
    if (results == 0) {
        return kDdErrInvalidParams;
    }
    zero(results, 4 * count);
    return kDdOk;
}

std::uint32_t devGetTexture() { // (this, stage, lplpTexture)
    Device *self = device(A32(0));
    const std::uint32_t out = clearedOut(A32(2));
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    Texture *found = nullptr;
    const HResult result = self->GetTexture(A32(1), &found);
    if (result != kDdOk || found == nullptr) {
        return result;
    }
    return handOut(out, found, GI::kTexture2);
}

std::uint32_t devSetTexture() { // (this, stage, texture)
    Device *self = device(A32(0));
    Texture *set = nullptr;
    if (self == nullptr || !optionalArg(A32(2), texture, set)) {
        return kDdErrInvalidObject;
    }
    return self->SetTexture(A32(1), set);
}

std::uint32_t devGetTextureStageState() { // (this, stage, state, value)
    Device *self = device(A32(0));
    const std::uint32_t out = A32(3);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    std::uint32_t value = 0;
    const HResult result = self->GetTextureStageState(A32(1), A32(2), value);
    if (result == kDdOk) {
        write32(out, value);
    }
    return result;
}

std::uint32_t devSetTextureStageState() { // (this, stage, state, value)
    Device *self = device(A32(0));
    return self == nullptr ? kDdErrInvalidObject
                           : self->SetTextureStageState(A32(1), A32(2), A32(3));
}

std::uint32_t devValidateDevice() { // (this, passes)
    Device *self = device(A32(0));
    const std::uint32_t out = A32(1);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    std::uint32_t passes = 0;
    const HResult result = self->ValidateDevice(passes);
    write32(out, passes);
    return result;
}

// ---- IDirect3DTexture2 ----

std::uint32_t texGetHandle() { // (this, device, handle)
    Texture *self = texture(A32(0));
    Device *on = nullptr;
    const std::uint32_t out = A32(2);
    if (self == nullptr || !optionalArg(A32(1), device, on)) {
        return kDdErrInvalidObject;
    }
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    std::uint32_t handle = 0;
    const HResult result = self->GetHandle(on, handle);
    write32(out, handle);
    return result;
}

std::uint32_t texPaletteChanged() { // (this, start, count)
    Texture *self = texture(A32(0));
    return self == nullptr ? kDdErrInvalidObject : self->PaletteChanged(A32(1), A32(2));
}

std::uint32_t texLoad() { // (this, source)
    Texture *self = texture(A32(0));
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    return self->Load(texture(A32(1)));
}

// ---- IDirect3DViewport3 ----

std::uint32_t vpInitialize() { // (this, direct3D)
    Viewport *self = viewport(A32(0));
    return self == nullptr ? kDdErrInvalidObject : self->Initialize();
}

std::uint32_t vpGetViewport() { // (this, desc)
    Viewport *self = viewport(A32(0));
    const std::uint32_t out = A32(1);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0 || read32(out) != kViewportSize) {
        return kDdErrInvalidParams;
    }
    ViewportV1 desc;
    const HResult result = self->GetViewport(desc);
    if (result == kDdOk) {
        writeViewport1(out, desc);
    }
    return result;
}

std::uint32_t vpSetViewport() { // (this, desc)
    Viewport *self = viewport(A32(0));
    const std::uint32_t desc = A32(1);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (desc == 0 || read32(desc) != kViewportSize) {
        return kDdErrInvalidParams;
    }
    return self->SetViewport(readViewport1(desc));
}

std::uint32_t vpUnsupported() { // TransformVertices, LightElements, lights, background depth
    return viewport(A32(0)) == nullptr ? kDdErrInvalidObject : kDdErrUnsupported;
}

std::uint32_t vpSetBackground() { // (this, material)
    Viewport *self = viewport(A32(0));
    return self == nullptr ? kDdErrInvalidObject : self->SetBackground(A32(1));
}

std::uint32_t vpGetBackground() { // (this, material, valid)
    Viewport *self = viewport(A32(0));
    const std::uint32_t materialOut = A32(1);
    const std::uint32_t validOut = A32(2);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (materialOut == 0 || validOut == 0) {
        return kDdErrInvalidParams;
    }
    std::uint32_t material = 0;
    bool valid = false;
    const HResult result = self->GetBackground(material, valid);
    write32(materialOut, material);
    write32(validOut, valid ? 1u : 0u);
    return result;
}

std::uint32_t vpGetBackgroundDepth() { // (this, lplpSurface, valid): there is none
    const std::uint32_t out = clearedOut(A32(1));
    const std::uint32_t validOut = A32(2);
    if (viewport(A32(0)) == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0 || validOut == 0) {
        return kDdErrInvalidParams;
    }
    write32(validOut, 0);
    return kDdOk;
}

std::uint32_t vpClear() { // (this, count, rects, flags)
    Viewport *self = viewport(A32(0));
    const std::uint32_t count = A32(1);
    const std::uint32_t rects = A32(2);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (count != 0 && rects == 0) {
        return kDdErrInvalidParams;
    }
    return self->Clear(readRects(rects, count), A32(3));
}

std::uint32_t vpNextLight() { // (this, light, lplpLight, flags): there are no lights
    clearedOut(A32(2));
    return viewport(A32(0)) == nullptr ? kDdErrInvalidObject : ddraw4::kDdErrNotFound;
}

std::uint32_t vpGetViewport2() { // (this, desc)
    Viewport *self = viewport(A32(0));
    const std::uint32_t out = A32(1);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0 || read32(out) != kViewportSize) {
        return kDdErrInvalidParams;
    }
    ViewportDesc desc;
    const HResult result = self->GetViewport2(desc);
    if (result == kDdOk) {
        writeViewport2(out, desc);
    }
    return result;
}

std::uint32_t vpSetViewport2() { // (this, desc)
    Viewport *self = viewport(A32(0));
    const std::uint32_t desc = A32(1);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (desc == 0 || read32(desc) != kViewportSize) {
        return kDdErrInvalidParams;
    }
    return self->SetViewport2(readViewport2(desc));
}

std::uint32_t vpClear2() { // (this, count, rects, flags, color, z, stencil)
    Viewport *self = viewport(A32(0));
    const std::uint32_t count = A32(1);
    const std::uint32_t rects = A32(2);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (count != 0 && rects == 0) {
        return kDdErrInvalidParams;
    }
    const std::uint32_t zBits = A32(5);
    float z = 0.0f;
    std::memcpy(&z, &zBits, 4);
    return self->Clear2(readRects(rects, count), A32(3), A32(4), z, A32(6));
}

// ---- the vtables, in slot order, with each row's stack bytes ----

#define SFCOP_IUNKNOWN(iface)                                                                      \
    {"sfcop!" iface "::QueryInterface", shim<queryInterface, 12>},                                 \
        {"sfcop!" iface "::AddRef", shim<addRef, 4>}, {                                            \
        "sfcop!" iface "::Release", shim<release, 4>                                               \
    }

const win32hle_shim kDirect3D3Methods[] = {
    SFCOP_IUNKNOWN("IDirect3D3"),
    {"sfcop!IDirect3D3::EnumDevices", shim<d3dEnumDevices, 12>},
    {"sfcop!IDirect3D3::CreateLight", shim<d3dUnsupportedCreate, 12>},
    {"sfcop!IDirect3D3::CreateMaterial", shim<d3dUnsupportedCreate, 12>},
    {"sfcop!IDirect3D3::CreateViewport", shim<d3dCreateViewport, 12>},
    {"sfcop!IDirect3D3::FindDevice", shim<d3dFindDevice, 12>},
    {"sfcop!IDirect3D3::CreateDevice", shim<d3dCreateDevice, 20>},
    {"sfcop!IDirect3D3::CreateVertexBuffer", shim<d3dCreateVertexBuffer, 20>},
    {"sfcop!IDirect3D3::EnumZBufferFormats", shim<d3dEnumZBufferFormats, 16>},
    {"sfcop!IDirect3D3::EvictManagedTextures", shim<d3dEvictManagedTextures, 4>},
    {nullptr, nullptr},
};

const win32hle_shim kDevice3Methods[] = {
    SFCOP_IUNKNOWN("IDirect3DDevice3"),
    {"sfcop!IDirect3DDevice3::GetCaps", shim<devGetCaps, 12>},
    {"sfcop!IDirect3DDevice3::GetStats", shim<devGetStats, 8>},
    {"sfcop!IDirect3DDevice3::AddViewport", shim<devAddViewport, 8>},
    {"sfcop!IDirect3DDevice3::DeleteViewport", shim<devDeleteViewport, 8>},
    {"sfcop!IDirect3DDevice3::NextViewport", shim<devNextViewport, 16>},
    {"sfcop!IDirect3DDevice3::EnumTextureFormats", shim<devEnumTextureFormats, 12>},
    {"sfcop!IDirect3DDevice3::BeginScene", shim<devBeginScene, 4>},
    {"sfcop!IDirect3DDevice3::EndScene", shim<devEndScene, 4>},
    {"sfcop!IDirect3DDevice3::GetDirect3D", shim<devGetDirect3D, 8>},
    {"sfcop!IDirect3DDevice3::SetCurrentViewport", shim<devSetCurrentViewport, 8>},
    {"sfcop!IDirect3DDevice3::GetCurrentViewport", shim<devGetCurrentViewport, 8>},
    {"sfcop!IDirect3DDevice3::SetRenderTarget", shim<devSetRenderTarget, 12>},
    {"sfcop!IDirect3DDevice3::GetRenderTarget", shim<devGetRenderTarget, 8>},
    {"sfcop!IDirect3DDevice3::Begin", shim<devUnsupported, 16>},
    {"sfcop!IDirect3DDevice3::BeginIndexed", shim<devUnsupported, 24>},
    {"sfcop!IDirect3DDevice3::Vertex", shim<devUnsupported, 8>},
    {"sfcop!IDirect3DDevice3::Index", shim<devUnsupported, 8>},
    {"sfcop!IDirect3DDevice3::End", shim<devUnsupported, 8>},
    {"sfcop!IDirect3DDevice3::GetRenderState", shim<devGetRenderState, 12>},
    {"sfcop!IDirect3DDevice3::SetRenderState", shim<devSetRenderState, 12>},
    {"sfcop!IDirect3DDevice3::GetLightState", shim<devGetLightState, 12>},
    {"sfcop!IDirect3DDevice3::SetLightState", shim<devSetLightState, 12>},
    {"sfcop!IDirect3DDevice3::SetTransform", shim<devSetTransform, 12>},
    {"sfcop!IDirect3DDevice3::GetTransform", shim<devGetTransform, 12>},
    {"sfcop!IDirect3DDevice3::MultiplyTransform", shim<devMultiplyTransform, 12>},
    {"sfcop!IDirect3DDevice3::DrawPrimitive", shim<devDrawPrimitive, 24>},
    {"sfcop!IDirect3DDevice3::DrawIndexedPrimitive", shim<devDrawIndexedPrimitive, 32>},
    {"sfcop!IDirect3DDevice3::SetClipStatus", shim<devSetClipStatus, 8>},
    {"sfcop!IDirect3DDevice3::GetClipStatus", shim<devGetClipStatus, 8>},
    {"sfcop!IDirect3DDevice3::DrawPrimitiveStrided", shim<devDrawPrimitiveStrided, 24>},
    {"sfcop!IDirect3DDevice3::DrawIndexedPrimitiveStrided",
     shim<devDrawIndexedPrimitiveStrided, 32>},
    {"sfcop!IDirect3DDevice3::DrawPrimitiveVB", shim<devUnsupported, 24>},
    {"sfcop!IDirect3DDevice3::DrawIndexedPrimitiveVB", shim<devUnsupported, 24>},
    {"sfcop!IDirect3DDevice3::ComputeSphereVisibility", shim<devComputeSphereVisibility, 24>},
    {"sfcop!IDirect3DDevice3::GetTexture", shim<devGetTexture, 12>},
    {"sfcop!IDirect3DDevice3::SetTexture", shim<devSetTexture, 12>},
    {"sfcop!IDirect3DDevice3::GetTextureStageState", shim<devGetTextureStageState, 16>},
    {"sfcop!IDirect3DDevice3::SetTextureStageState", shim<devSetTextureStageState, 16>},
    {"sfcop!IDirect3DDevice3::ValidateDevice", shim<devValidateDevice, 8>},
    {nullptr, nullptr},
};

const win32hle_shim kTexture2Methods[] = {
    SFCOP_IUNKNOWN("IDirect3DTexture2"),
    {"sfcop!IDirect3DTexture2::GetHandle", shim<texGetHandle, 12>},
    {"sfcop!IDirect3DTexture2::PaletteChanged", shim<texPaletteChanged, 12>},
    {"sfcop!IDirect3DTexture2::Load", shim<texLoad, 8>},
    {nullptr, nullptr},
};

const win32hle_shim kViewport3Methods[] = {
    SFCOP_IUNKNOWN("IDirect3DViewport3"),
    {"sfcop!IDirect3DViewport3::Initialize", shim<vpInitialize, 8>},
    {"sfcop!IDirect3DViewport3::GetViewport", shim<vpGetViewport, 8>},
    {"sfcop!IDirect3DViewport3::SetViewport", shim<vpSetViewport, 8>},
    {"sfcop!IDirect3DViewport3::TransformVertices", shim<vpUnsupported, 20>},
    {"sfcop!IDirect3DViewport3::LightElements", shim<vpUnsupported, 12>},
    {"sfcop!IDirect3DViewport3::SetBackground", shim<vpSetBackground, 8>},
    {"sfcop!IDirect3DViewport3::GetBackground", shim<vpGetBackground, 12>},
    {"sfcop!IDirect3DViewport3::SetBackgroundDepth", shim<vpUnsupported, 8>},
    {"sfcop!IDirect3DViewport3::GetBackgroundDepth", shim<vpGetBackgroundDepth, 12>},
    {"sfcop!IDirect3DViewport3::Clear", shim<vpClear, 16>},
    {"sfcop!IDirect3DViewport3::AddLight", shim<vpUnsupported, 8>},
    {"sfcop!IDirect3DViewport3::DeleteLight", shim<vpUnsupported, 8>},
    {"sfcop!IDirect3DViewport3::NextLight", shim<vpNextLight, 16>},
    {"sfcop!IDirect3DViewport3::GetViewport2", shim<vpGetViewport2, 8>},
    {"sfcop!IDirect3DViewport3::SetViewport2", shim<vpSetViewport2, 8>},
    {"sfcop!IDirect3DViewport3::SetBackgroundDepth2", shim<vpUnsupported, 8>},
    {"sfcop!IDirect3DViewport3::GetBackgroundDepth2", shim<vpGetBackgroundDepth, 12>},
    {"sfcop!IDirect3DViewport3::Clear2", shim<vpClear2, 28>},
    {nullptr, nullptr},
};

#undef SFCOP_IUNKNOWN

std::uint32_t makeView(GI iface) {
    static std::map<GI, std::uint32_t> vtables;
    std::uint32_t &vtable = vtables[iface];
    if (vtable == 0) {
        switch (iface) {
        case GI::kDirect3D3:
            vtable = hle_com_vtable(kDirect3D3Methods);
            break;
        case GI::kDevice3:
            vtable = hle_com_vtable(kDevice3Methods);
            break;
        case GI::kViewport3:
            vtable = hle_com_vtable(kViewport3Methods);
            break;
        default:
            vtable = hle_com_vtable(kTexture2Methods);
            break;
        }
    }
    return hle_com_new(vtable, 8);
}

// Installed when the program starts (this file is compiled only into the client).
[[maybe_unused]] const bool kInstalled = [] {
    GuestViews &all = GuestViews::Instance();
    all.SetMaker(GI::kDirect3D3, GI::kTexture2, makeView);
    all.SetQueryHook(queryView);
    return true;
}();

} // namespace

} // namespace sfcop::hle::d3d3

// Direct3D is reached through QueryInterface only (see the top of the file).
extern "C" const win32hle_shim sfcop_hle_d3d3[] = {
    {nullptr, nullptr},
};

#endif // __has_include("win32hle.h")
