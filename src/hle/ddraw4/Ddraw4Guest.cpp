// The guest shims of the DirectDraw module (src/hle/README.md, "Implementing a module"): the
// DDRAW.DLL exports and the methods of IDirectDraw, IDirectDraw2, IDirectDraw4,
// IDirectDrawSurface3, IDirectDrawSurface4, IDirectDrawPalette and IDirectDrawClipper, one shim
// per row of src/hle/surface.tsv. Each reads its arguments from the guest stack, calls the module
// and writes its out-parameters back; shim<Method, Bytes> purges the row's stack bytes.
//
// TEMPORARY: until the build compiles *Guest.* files only with the lift, this file is also
// compiled into sfcop_src, where the lift's headers are not on the include path; it is then
// empty.
#if __has_include("win32hle.h")

#include "hle/ddraw4/Clipper.h"
#include "hle/ddraw4/DirectDraw.h"
#include "hle/ddraw4/GuestAccess.h"
#include "hle/ddraw4/GuestViews.h"
#include "hle/ddraw4/Palette.h"
#include "hle/ddraw4/Surface.h"

#include <cstring>
#include <iterator>
#include <map>
#include <vector>

// TEMPORARY: the boot platform's table that answers DirectDrawCreateClipper (src/app/boot/), for
// the hand-off below.
extern "C" const win32hle_shim sfcop_hle_misc[];

namespace sfcop::hle::ddraw4 {

namespace {

using namespace guest;
using GI = GuestInterface;

GuestViews &views() {
    return GuestViews::Instance();
}

// Runs `Method` and returns its result to the guest, purging `kStackBytes` (the row's
// stack_bytes) of arguments.
template <std::uint32_t (*Method)(), std::uint32_t kStackBytes> void shim() {
    const std::uint32_t result = Method();
    RET(result, kStackBytes / 4);
}

// ---- the host objects behind the game's addresses ----

DirectDraw *directDraw(std::uint32_t address) {
    return static_cast<DirectDraw *>(views().Host(address, GI::kDirectDraw, GI::kDirectDraw4));
}

Surface *surface(std::uint32_t address) {
    return static_cast<Surface *>(views().Host(address, GI::kSurface3, GI::kSurface4));
}

Palette *palette(std::uint32_t address) {
    return static_cast<Palette *>(views().Host(address, GI::kPalette, GI::kPalette));
}

Clipper *clipper(std::uint32_t address) {
    return static_cast<Clipper *>(views().Host(address, GI::kClipper, GI::kClipper));
}

// An optional object argument: true and the object (or null for a null address), or false for an
// address that is not one.
template <class T> bool optionalArg(std::uint32_t address, T *(*find)(std::uint32_t), T *&out) {
    out = address == 0 ? nullptr : find(address);
    return address == 0 || out != nullptr;
}

// Hands the game a view of `object`, which holds the reference being handed out; on failure the
// reference is dropped and *out stays null.
HResult handOut(std::uint32_t out, ComObject *object, GI iface) {
    const std::uint32_t view = views().View(object, iface);
    if (view == 0) {
        object->Release();
        return kDdErrOutOfMemory;
    }
    write32(out, view);
    return kDdOk;
}

constexpr GI surfaceViewFor(GI directDrawView) {
    return directDrawView == GI::kDirectDraw4 ? GI::kSurface4 : GI::kSurface3;
}

std::optional<RectL> optionalRect(std::uint32_t address) {
    if (address == 0) {
        return std::nullopt;
    }
    return readRect(address);
}

const RectL *pointer(const std::optional<RectL> &rect) {
    return rect ? &*rect : nullptr;
}

// Surface pixels in guest memory, where Lock can hand the game a pointer.
class GuestPixelMemory final : public PixelMemory {
public:
    std::uint8_t *Allocate(std::size_t bytes) override {
        if (bytes > UINT32_MAX) {
            return nullptr;
        }
        return static_cast<std::uint8_t *>(hle_alloc(static_cast<std::uint32_t>(bytes)));
    }
    void Free(std::uint8_t *block) override {
        if (block != nullptr) {
            hle_free(block);
        }
    }
};

GuestPixelMemory &pixelMemory() {
    static GuestPixelMemory memory;
    return memory;
}

// The DCs handed out by GetDC, until ReleaseDC.
std::map<std::uint32_t, Surface *> &deviceContexts() {
    static std::map<std::uint32_t, Surface *> contexts;
    return contexts;
}

// Data the game attached with SetPrivateData.
std::map<std::pair<const ComObject *, Guid>, std::vector<std::uint8_t>> &privateData() {
    static std::map<std::pair<const ComObject *, Guid>, std::vector<std::uint8_t>> data;
    return data;
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
    // IDirect3D3 and IDirect3DTexture2 come from the Direct3D module's hook.
    const std::uint32_t result = views().Query(*view->host, view->iface, wanted);
    if (result == 0) {
        return kENoInterface;
    }
    write32(out, result);
    return kDdOk;
}

std::uint32_t addRef() { // (this)
    const auto view = views().Find(A32(0));
    return view ? view->host->AddRef() : 0;
}

std::uint32_t release() { // (this); the views go with their host object (onDestroyed)
    const auto view = views().Find(A32(0));
    return view ? view->host->Release() : 0;
}

// ---- IDirectDraw, IDirectDraw2, IDirectDraw4 ----

std::uint32_t ddCompact() { // (this)
    DirectDraw *dd = directDraw(A32(0));
    return dd == nullptr ? kDdErrInvalidObject : dd->Compact();
}

std::uint32_t ddCreateClipper() { // (this, flags, lplpClipper, outer)
    DirectDraw *dd = directDraw(A32(0));
    const std::uint32_t out = A32(2);
    if (dd == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    write32(out, 0);
    if (A32(3) != 0) {
        return kClassENoAggregation;
    }
    Clipper *created = nullptr;
    const HResult result = dd->CreateClipper(A32(1), &created);
    return result != kDdOk ? result : handOut(out, created, GI::kClipper);
}

std::uint32_t ddCreatePalette() { // (this, flags, entries, lplpPalette, outer)
    DirectDraw *dd = directDraw(A32(0));
    const std::uint32_t flags = A32(1);
    const std::uint32_t entries = A32(2);
    const std::uint32_t out = A32(3);
    if (dd == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    write32(out, 0);
    if (A32(4) != 0) {
        return kClassENoAggregation;
    }
    std::vector<PaletteColor> colors;
    if (entries != 0 && (flags & kDdpcaps8Bit) != 0) {
        colors.resize(256);
        for (std::uint32_t i = 0; i < 256; ++i) {
            colors[i] = readPaletteColor(entries + 4 * i);
        }
    }
    Palette *created = nullptr;
    const HResult result =
        dd->CreatePalette(flags, colors.empty() ? nullptr : colors.data(), &created);
    return result != kDdOk ? result : handOut(out, created, GI::kPalette);
}

template <GI kView> std::uint32_t ddCreateSurface() { // (this, desc, lplpSurface, outer)
    DirectDraw *dd = directDraw(A32(0));
    const std::uint32_t descAddress = A32(1);
    const std::uint32_t out = A32(2);
    if (dd == nullptr) {
        return kDdErrInvalidObject;
    }
    if (descAddress == 0 || out == 0) {
        return kDdErrInvalidParams;
    }
    write32(out, 0);
    if (A32(3) != 0) {
        return kClassENoAggregation;
    }
    const auto desc = readSurfaceDesc(descAddress);
    if (!desc) {
        return kDdErrInvalidParams;
    }
    Surface *created = nullptr;
    const HResult result = dd->CreateSurface(*desc, &created);
    return result != kDdOk ? result : handOut(out, created, surfaceViewFor(kView));
}

std::uint32_t ddDuplicateSurface() { // (this, surface, lplpDuplicate): not served
    return directDraw(A32(0)) == nullptr ? kDdErrInvalidObject : kDdErrUnsupported;
}

template <GI kView> std::uint32_t ddEnumDisplayModes() { // (this, flags, desc, context, callback)
    DirectDraw *dd = directDraw(A32(0));
    const std::uint32_t flags = A32(1);
    const std::uint32_t filterAddress = A32(2);
    const std::uint32_t context = A32(3);
    const std::uint32_t callback = A32(4);
    if (dd == nullptr) {
        return kDdErrInvalidObject;
    }
    if (callback == 0) {
        return kDdErrInvalidParams;
    }
    std::optional<SurfaceDesc> filter;
    if (filterAddress != 0) {
        filter = readSurfaceDesc(filterAddress);
        if (!filter) {
            return kDdErrInvalidParams;
        }
    }
    std::vector<SurfaceDesc> modes;
    const HResult result = dd->EnumDisplayModes(flags, filter ? &*filter : nullptr, modes);
    if (result != kDdOk) {
        return result;
    }
    const std::uint32_t size = kView == GI::kDirectDraw4 ? kSurfaceDesc2Size : kSurfaceDescSize;
    for (const SurfaceDesc &mode : modes) {
        Scratch scratch;
        const std::uint32_t desc = scratch.Take(size);
        writeSurfaceDesc(desc, mode, size);
        if (callGuest(callback, {desc, context}) == kDdenumretCancel) {
            break;
        }
    }
    return kDdOk;
}

std::uint32_t ddEnumSurfaces() { // (this, flags, desc, context, callback): enumerates nothing
    return directDraw(A32(0)) == nullptr ? kDdErrInvalidObject : kDdOk;
}

std::uint32_t ddFlipToGdiSurface() { // (this)
    DirectDraw *dd = directDraw(A32(0));
    return dd == nullptr ? kDdErrInvalidObject : dd->FlipToGdiSurface();
}

// DDCAPS up to the dwSize the game gave (DDCAPS_DX6 is 380 bytes; ddsOldCaps ends at 136).
void writeDdCaps(std::uint32_t address, const DdCaps &caps) {
    const std::uint32_t size = read32(address);
    zero(address + 4, size - 4);
    const auto put = [&](std::uint32_t offset, std::uint32_t value) {
        if (offset + 4 <= size) {
            write32(address + offset, value);
        }
    };
    put(4, caps.caps);
    put(8, caps.caps2);
    put(12, caps.colorKeyCaps);
    put(16, caps.fxCaps);
    put(24, caps.paletteCaps);
    put(56, caps.zBufferBitDepths);
    put(60, caps.vidMemTotal);
    put(64, caps.vidMemFree);
    for (std::uint32_t i = 0; i < 8; ++i) {
        put(100 + 4 * i, caps.rops[i]);
    }
    put(132, caps.surfaceCaps.caps); // ddsOldCaps
    put(364, caps.surfaceCaps.caps); // ddsCaps (DDSCAPS2)
    put(368, caps.surfaceCaps.caps2);
    put(372, caps.surfaceCaps.caps3);
    put(376, caps.surfaceCaps.caps4);
}

bool isDdCapsSize(std::uint32_t size) {
    return size >= 136 && size <= 380;
}

std::uint32_t ddGetCaps() { // (this, driverCaps, emulationCaps)
    DirectDraw *dd = directDraw(A32(0));
    const std::uint32_t driver = A32(1);
    const std::uint32_t emulation = A32(2);
    if (dd == nullptr) {
        return kDdErrInvalidObject;
    }
    if ((driver == 0 && emulation == 0) || (driver != 0 && !isDdCapsSize(read32(driver))) ||
        (emulation != 0 && !isDdCapsSize(read32(emulation)))) {
        return kDdErrInvalidParams;
    }
    DdCaps driverCaps;
    DdCaps emulationCaps;
    const HResult result = dd->GetCaps(&driverCaps, &emulationCaps);
    if (result != kDdOk) {
        return result;
    }
    if (driver != 0) {
        writeDdCaps(driver, driverCaps);
    }
    if (emulation != 0) {
        writeDdCaps(emulation, emulationCaps);
    }
    return kDdOk;
}

std::uint32_t ddGetDisplayMode() { // (this, desc)
    DirectDraw *dd = directDraw(A32(0));
    const std::uint32_t desc = A32(1);
    if (dd == nullptr) {
        return kDdErrInvalidObject;
    }
    if (desc == 0 || !isSurfaceDescSize(read32(desc))) {
        return kDdErrInvalidParams;
    }
    SurfaceDesc mode;
    const HResult result = dd->GetDisplayMode(mode);
    if (result == kDdOk) {
        writeSurfaceDesc(desc, mode, read32(desc));
    }
    return result;
}

std::uint32_t ddGetFourCcCodes() { // (this, count, codes): no FourCC formats
    if (directDraw(A32(0)) == nullptr) {
        return kDdErrInvalidObject;
    }
    writeOut(A32(1), 0);
    return kDdOk;
}

template <GI kView> std::uint32_t ddGetGdiSurface() { // (this, lplpSurface)
    DirectDraw *dd = directDraw(A32(0));
    const std::uint32_t out = A32(1);
    if (dd == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    write32(out, 0);
    Surface *primary = nullptr;
    const HResult result = dd->GetGdiSurface(&primary);
    return result != kDdOk ? result : handOut(out, primary, surfaceViewFor(kView));
}

std::uint32_t ddGetMonitorFrequency() { // (this, frequency)
    DirectDraw *dd = directDraw(A32(0));
    const std::uint32_t out = A32(1);
    if (dd == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    std::uint32_t hertz = 0;
    const HResult result = dd->GetMonitorFrequency(hertz);
    write32(out, hertz);
    return result;
}

std::uint32_t ddGetScanLine() { // (this, line)
    DirectDraw *dd = directDraw(A32(0));
    const std::uint32_t out = A32(1);
    if (dd == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    std::uint32_t line = 0;
    const HResult result = dd->GetScanLine(line);
    write32(out, line);
    return result;
}

std::uint32_t ddGetVerticalBlankStatus() { // (this, isInVB)
    DirectDraw *dd = directDraw(A32(0));
    const std::uint32_t out = A32(1);
    if (dd == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    bool inBlank = false;
    const HResult result = dd->GetVerticalBlankStatus(inBlank);
    write32(out, inBlank ? 1u : 0u);
    return result;
}

std::uint32_t ddInitialize() { // (this, guid)
    DirectDraw *dd = directDraw(A32(0));
    return dd == nullptr ? kDdErrInvalidObject : dd->Initialize();
}

std::uint32_t ddRestoreDisplayMode() { // (this)
    DirectDraw *dd = directDraw(A32(0));
    return dd == nullptr ? kDdErrInvalidObject : dd->RestoreDisplayMode();
}

std::uint32_t ddSetCooperativeLevel() { // (this, hwnd, flags)
    DirectDraw *dd = directDraw(A32(0));
    return dd == nullptr ? kDdErrInvalidObject : dd->SetCooperativeLevel(A32(1), A32(2));
}

std::uint32_t ddSetDisplayMode1() { // (this, width, height, bpp)
    DirectDraw *dd = directDraw(A32(0));
    return dd == nullptr ? kDdErrInvalidObject : dd->SetDisplayMode(A32(1), A32(2), A32(3), 0, 0);
}

std::uint32_t ddSetDisplayMode2() { // (this, width, height, bpp, refreshRate, flags)
    DirectDraw *dd = directDraw(A32(0));
    return dd == nullptr ? kDdErrInvalidObject
                         : dd->SetDisplayMode(A32(1), A32(2), A32(3), A32(4), A32(5));
}

std::uint32_t ddWaitForVerticalBlank() { // (this, flags, event)
    DirectDraw *dd = directDraw(A32(0));
    return dd == nullptr ? kDdErrInvalidObject : dd->WaitForVerticalBlank(A32(1));
}

template <GI kView> std::uint32_t ddGetAvailableVidMem() { // (this, caps, total, free)
    DirectDraw *dd = directDraw(A32(0));
    const std::uint32_t capsAddress = A32(1);
    const std::uint32_t totalOut = A32(2);
    const std::uint32_t freeOut = A32(3);
    if (dd == nullptr) {
        return kDdErrInvalidObject;
    }
    if (capsAddress == 0) {
        return kDdErrInvalidParams;
    }
    const SurfaceCaps caps = readCaps(capsAddress, kView == GI::kDirectDraw4);
    std::uint32_t total = 0;
    std::uint32_t free = 0;
    const HResult result = dd->GetAvailableVidMem(caps, &total, &free);
    if (result == kDdOk) {
        writeOut(totalOut, total);
        writeOut(freeOut, free);
    }
    return result;
}

std::uint32_t ddGetSurfaceFromDc() { // (this, hdc, lplpSurface)
    const std::uint32_t out = A32(2);
    if (directDraw(A32(0)) == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    write32(out, 0);
    const auto found = deviceContexts().find(A32(1));
    if (found == deviceContexts().end()) {
        return kDdErrNotFound;
    }
    found->second->AddRef();
    return handOut(out, found->second, GI::kSurface4);
}

std::uint32_t ddRestoreAllSurfaces() { // (this)
    DirectDraw *dd = directDraw(A32(0));
    return dd == nullptr ? kDdErrInvalidObject : dd->RestoreAllSurfaces();
}

std::uint32_t ddTestCooperativeLevel() { // (this)
    DirectDraw *dd = directDraw(A32(0));
    return dd == nullptr ? kDdErrInvalidObject : dd->TestCooperativeLevel();
}

// DDDEVICEIDENTIFIER: szDriver[512], szDescription[512], liDriverVersion, dwVendorId,
// dwDeviceId, dwSubSysId, dwRevision, guidDeviceIdentifier (1064 bytes).
std::uint32_t ddGetDeviceIdentifier() { // (this, identifier, flags)
    DirectDraw *dd = directDraw(A32(0));
    const std::uint32_t out = A32(1);
    if (dd == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    DeviceIdentifier identifier;
    const HResult result = dd->GetDeviceIdentifier(A32(2), identifier);
    if (result == kDdOk) {
        zero(out, 1064);
        writeString(out, 512, identifier.driver);
        writeString(out + 512, 512, identifier.description);
        write32(out + 1032, identifier.vendorId);
        write32(out + 1036, identifier.deviceId);
    }
    return result;
}

// ---- IDirectDrawSurface3, IDirectDrawSurface4 ----

std::uint32_t srfAddAttachedSurface() { // (this, surface)
    Surface *self = surface(A32(0));
    Surface *attached = surface(A32(1));
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    return attached == nullptr ? kDdErrInvalidParams : self->AddAttachedSurface(attached);
}

std::uint32_t srfUnsupported() { // overlays, BltBatch, SetSurfaceDesc: not served
    return surface(A32(0)) == nullptr ? kDdErrInvalidObject : kDdErrUnsupported;
}

std::uint32_t srfOk() { // GetBltStatus, GetFlipStatus, PageLock, PageUnlock, EnumOverlayZOrders
    return surface(A32(0)) == nullptr ? kDdErrInvalidObject : kDdOk;
}

// DDBLTFX: dwDDFX at 4, dwROP at 8, dwFillColor at 80, ddckDestColorkey at 84,
// ddckSrcColorkey at 92.
BltFx readBltFx(std::uint32_t address) {
    BltFx fx;
    fx.ddfx = read32(address + 4);
    fx.rop = read32(address + 8);
    fx.fillColor = read32(address + 80);
    fx.destKey = readKey(address + 84);
    fx.srcKey = readKey(address + 92);
    return fx;
}

std::uint32_t srfBlt() { // (this, destRect, source, sourceRect, flags, fx)
    Surface *self = surface(A32(0));
    const auto destination = optionalRect(A32(1));
    const auto sourceRect = optionalRect(A32(3));
    const std::uint32_t flags = A32(4);
    const std::uint32_t fxAddress = A32(5);
    Surface *source = nullptr;
    if (self == nullptr || !optionalArg(A32(2), surface, source)) {
        return kDdErrInvalidObject;
    }
    std::optional<BltFx> fx;
    if (fxAddress != 0) {
        fx = readBltFx(fxAddress);
    }
    return self->Blt(pointer(destination), source, pointer(sourceRect), flags, fx ? &*fx : nullptr);
}

std::uint32_t srfBltFast() { // (this, x, y, source, sourceRect, transfer)
    Surface *self = surface(A32(0));
    Surface *source = surface(A32(3));
    const auto sourceRect = optionalRect(A32(4));
    if (self == nullptr || source == nullptr) {
        return kDdErrInvalidObject;
    }
    return self->BltFast(A32(1), A32(2), source, pointer(sourceRect), A32(5));
}

std::uint32_t srfDeleteAttachedSurface() { // (this, flags, surface)
    Surface *self = surface(A32(0));
    Surface *attached = nullptr;
    if (self == nullptr || !optionalArg(A32(2), surface, attached)) {
        return kDdErrInvalidObject;
    }
    return self->DeleteAttachedSurface(A32(1), attached);
}

template <GI kView> std::uint32_t srfEnumAttachedSurfaces() { // (this, context, callback)
    Surface *self = surface(A32(0));
    const std::uint32_t context = A32(1);
    const std::uint32_t callback = A32(2);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (callback == 0) {
        return kDdErrInvalidParams;
    }
    const std::uint32_t size = kView == GI::kSurface4 ? kSurfaceDesc2Size : kSurfaceDescSize;
    for (Surface *attached : self->AttachedSurfaces()) {
        attached->AddRef(); // the callback's reference, which it releases
        const std::uint32_t view = views().View(attached, kView);
        if (view == 0) {
            attached->Release();
            return kDdErrOutOfMemory;
        }
        SurfaceDesc desc;
        (void)attached->GetSurfaceDesc(desc);
        Scratch scratch;
        const std::uint32_t descAddress = scratch.Take(size);
        writeSurfaceDesc(descAddress, desc, size);
        if (callGuest(callback, {view, descAddress, context}) == kDdenumretCancel) {
            break;
        }
    }
    return kDdOk;
}

std::uint32_t srfFlip() { // (this, target, flags)
    Surface *self = surface(A32(0));
    Surface *target = nullptr;
    if (self == nullptr || !optionalArg(A32(1), surface, target)) {
        return kDdErrInvalidObject;
    }
    return self->Flip(target, A32(2));
}

template <GI kView> std::uint32_t srfGetAttachedSurface() { // (this, caps, lplpSurface)
    Surface *self = surface(A32(0));
    const std::uint32_t capsAddress = A32(1);
    const std::uint32_t out = A32(2);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (capsAddress == 0 || out == 0) {
        return kDdErrInvalidParams;
    }
    write32(out, 0);
    Surface *found = nullptr;
    const HResult result =
        self->GetAttachedSurface(readCaps(capsAddress, kView == GI::kSurface4), &found);
    return result != kDdOk ? result : handOut(out, found, kView);
}

template <GI kView> std::uint32_t srfGetCaps() { // (this, caps)
    Surface *self = surface(A32(0));
    const std::uint32_t out = A32(1);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    SurfaceCaps caps;
    const HResult result = self->GetCaps(caps);
    if (result == kDdOk) {
        writeCaps(out, caps, kView == GI::kSurface4);
    }
    return result;
}

std::uint32_t srfGetClipper() { // (this, lplpClipper)
    Surface *self = surface(A32(0));
    const std::uint32_t out = A32(1);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    write32(out, 0);
    Clipper *found = nullptr;
    const HResult result = self->GetClipper(&found);
    return result != kDdOk ? result : handOut(out, found, GI::kClipper);
}

std::uint32_t srfGetColorKey() { // (this, flags, key)
    Surface *self = surface(A32(0));
    const std::uint32_t out = A32(2);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    ColorKey key;
    const HResult result = self->GetColorKey(A32(1), key);
    if (result == kDdOk) {
        writeKey(out, key);
    }
    return result;
}

std::uint32_t srfGetDc() { // (this, hdc)
    Surface *self = surface(A32(0));
    const std::uint32_t out = A32(1);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    SurfaceDesc desc;
    const HResult result = self->GetDC(desc);
    if (result != kDdOk) {
        return result;
    }
    const std::uint32_t dc = hle_gdi_surface_dc(desc.surface, static_cast<int>(desc.width),
                                                static_cast<int>(desc.height), desc.pitch,
                                                static_cast<int>(desc.pixelFormat.bitCount));
    if (dc == 0) {
        (void)self->ReleaseDC();
        return kDdErrGeneric;
    }
    deviceContexts()[dc] = self;
    write32(out, dc);
    return kDdOk;
}

std::uint32_t srfGetPalette() { // (this, lplpPalette)
    Surface *self = surface(A32(0));
    const std::uint32_t out = A32(1);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    write32(out, 0);
    Palette *found = nullptr;
    const HResult result = self->GetPalette(&found);
    return result != kDdOk ? result : handOut(out, found, GI::kPalette);
}

std::uint32_t srfGetPixelFormat() { // (this, pixelFormat)
    Surface *self = surface(A32(0));
    const std::uint32_t out = A32(1);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    PixelFormatDesc format;
    const HResult result = self->GetPixelFormat(format);
    if (result == kDdOk) {
        writePixelFormat(out, format);
    }
    return result;
}

std::uint32_t srfGetSurfaceDesc() { // (this, desc)
    Surface *self = surface(A32(0));
    const std::uint32_t out = A32(1);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0 || !isSurfaceDescSize(read32(out))) {
        return kDdErrInvalidParams;
    }
    SurfaceDesc desc;
    const HResult result = self->GetSurfaceDesc(desc);
    if (result == kDdOk) {
        writeSurfaceDesc(out, desc, read32(out));
    }
    return result;
}

std::uint32_t srfInitialize() { // (this, directDraw, desc)
    return surface(A32(0)) == nullptr ? kDdErrInvalidObject : kDdErrAlreadyInitialized;
}

std::uint32_t srfIsLost() { // (this)
    Surface *self = surface(A32(0));
    return self == nullptr ? kDdErrInvalidObject : self->IsLost();
}

std::uint32_t srfLock() { // (this, rect, desc, flags, event)
    Surface *self = surface(A32(0));
    const auto rect = optionalRect(A32(1));
    const std::uint32_t out = A32(2);
    const std::uint32_t flags = A32(3);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0 || !isSurfaceDescSize(read32(out))) {
        return kDdErrInvalidParams;
    }
    SurfaceDesc desc;
    const HResult result = self->Lock(pointer(rect), desc, flags);
    if (result == kDdOk) {
        writeSurfaceDesc(out, desc, read32(out));
    }
    return result;
}

std::uint32_t srfReleaseDc() { // (this, hdc)
    Surface *self = surface(A32(0));
    const std::uint32_t dc = A32(1);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    const auto found = deviceContexts().find(dc);
    if (found == deviceContexts().end() || found->second != self) {
        return kDdErrNoDc;
    }
    deviceContexts().erase(found);
    hle_gdi_release_dc(dc);
    return self->ReleaseDC();
}

std::uint32_t srfRestore() { // (this)
    Surface *self = surface(A32(0));
    return self == nullptr ? kDdErrInvalidObject : self->Restore();
}

std::uint32_t srfSetClipper() { // (this, clipper)
    Surface *self = surface(A32(0));
    Clipper *attached = nullptr;
    if (self == nullptr || !optionalArg(A32(1), clipper, attached)) {
        return kDdErrInvalidObject;
    }
    return self->SetClipper(attached);
}

std::uint32_t srfSetColorKey() { // (this, flags, key)
    Surface *self = surface(A32(0));
    const std::uint32_t keyAddress = A32(2);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    std::optional<ColorKey> key;
    if (keyAddress != 0) {
        key = readKey(keyAddress);
    }
    return self->SetColorKey(A32(1), key ? &*key : nullptr);
}

std::uint32_t srfSetPalette() { // (this, palette)
    Surface *self = surface(A32(0));
    Palette *attached = nullptr;
    if (self == nullptr || !optionalArg(A32(1), palette, attached)) {
        return kDdErrInvalidObject;
    }
    return self->SetPalette(attached);
}

std::uint32_t srfUnlock() { // (this, rect or surface pointer)
    Surface *self = surface(A32(0));
    return self == nullptr ? kDdErrInvalidObject : self->Unlock();
}

template <GI kView> std::uint32_t srfGetDdInterface() { // (this, lplpDirectDraw)
    Surface *self = surface(A32(0));
    const std::uint32_t out = A32(1);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    write32(out, 0);
    DirectDraw &owner = self->Owner();
    owner.AddRef();
    return handOut(out, &owner, kView == GI::kSurface4 ? GI::kDirectDraw4 : GI::kDirectDraw);
}

// DDSPD_IUNKNOWNPOINTER: the data is an interface to hold, not bytes.
constexpr std::uint32_t kPrivateDataInterface = 0x1;

std::uint32_t srfSetPrivateData() { // (this, guid, data, size, flags)
    Surface *self = surface(A32(0));
    const std::uint32_t guid = A32(1);
    const std::uint32_t data = A32(2);
    const std::uint32_t size = A32(3);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (guid == 0 || (data == 0 && size != 0)) {
        return kDdErrInvalidParams;
    }
    if ((A32(4) & kPrivateDataInterface) != 0) {
        return kDdErrUnsupported;
    }
    std::vector<std::uint8_t> bytes(size);
    copyIn(bytes.data(), data, size);
    privateData()[{self, readGuid(guid)}] = std::move(bytes);
    return kDdOk;
}

std::uint32_t srfGetPrivateData() { // (this, guid, buffer, size)
    Surface *self = surface(A32(0));
    const std::uint32_t guid = A32(1);
    const std::uint32_t buffer = A32(2);
    const std::uint32_t sizeAddress = A32(3);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (guid == 0 || sizeAddress == 0) {
        return kDdErrInvalidParams;
    }
    const auto found = privateData().find({self, readGuid(guid)});
    if (found == privateData().end()) {
        return kDdErrNotFound;
    }
    const auto size = static_cast<std::uint32_t>(found->second.size());
    if (buffer == 0 || read32(sizeAddress) < size) {
        write32(sizeAddress, size);
        return kDdErrMoreData;
    }
    copyOut(buffer, found->second.data(), size);
    write32(sizeAddress, size);
    return kDdOk;
}

std::uint32_t srfFreePrivateData() { // (this, guid)
    Surface *self = surface(A32(0));
    const std::uint32_t guid = A32(1);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (guid == 0) {
        return kDdErrInvalidParams;
    }
    return privateData().erase({self, readGuid(guid)}) == 0 ? kDdErrNotFound : kDdOk;
}

std::uint32_t srfGetUniquenessValue() { // (this, value)
    Surface *self = surface(A32(0));
    const std::uint32_t out = A32(1);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    write32(out, self->UniquenessValue());
    return kDdOk;
}

std::uint32_t srfChangeUniquenessValue() { // (this)
    Surface *self = surface(A32(0));
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    self->ChangeUniquenessValue();
    return kDdOk;
}

// ---- IDirectDrawPalette ----

std::uint32_t palGetCaps() { // (this, caps)
    Palette *self = palette(A32(0));
    const std::uint32_t out = A32(1);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    std::uint32_t caps = 0;
    const HResult result = self->GetCaps(caps);
    write32(out, caps);
    return result;
}

std::uint32_t palGetEntries() { // (this, flags, start, count, entries)
    Palette *self = palette(A32(0));
    const std::uint32_t start = A32(2);
    const std::uint32_t count = A32(3);
    const std::uint32_t out = A32(4);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0 || count > 256) {
        return kDdErrInvalidParams;
    }
    std::vector<PaletteColor> colors(count);
    const HResult result = self->GetEntries(A32(1), start, count, colors.data());
    if (result == kDdOk) {
        for (std::uint32_t i = 0; i < count; ++i) {
            writePaletteColor(out + 4 * i, colors[i]);
        }
    }
    return result;
}

std::uint32_t palInitialize() { // (this, directDraw, flags, entries)
    Palette *self = palette(A32(0));
    return self == nullptr ? kDdErrInvalidObject : self->Initialize();
}

std::uint32_t palSetEntries() { // (this, flags, start, count, entries)
    Palette *self = palette(A32(0));
    const std::uint32_t start = A32(2);
    const std::uint32_t count = A32(3);
    const std::uint32_t entries = A32(4);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (entries == 0 || count > 256) {
        return kDdErrInvalidParams;
    }
    std::vector<PaletteColor> colors(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        colors[i] = readPaletteColor(entries + 4 * i);
    }
    return self->SetEntries(A32(1), start, count, colors.data());
}

// ---- IDirectDrawClipper ----

// RGNDATA: a 32-byte RGNDATAHEADER (dwSize, iType, nCount, nRgnSize, rcBound), then the rects.
constexpr std::uint32_t kRegionHeaderSize = 32;
constexpr std::uint32_t kRegionRectangles = 1; // RDH_RECTANGLES

std::uint32_t clpGetClipList() { // (this, clipRect, clipList, size)
    Clipper *self = clipper(A32(0));
    const auto clip = optionalRect(A32(1));
    const std::uint32_t list = A32(2);
    const std::uint32_t sizeAddress = A32(3);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (sizeAddress == 0) {
        return kDdErrInvalidParams;
    }
    std::vector<RectL> rects;
    const HResult result = self->GetClipList(pointer(clip), rects);
    if (result != kDdOk) {
        return result;
    }
    const auto count = static_cast<std::uint32_t>(rects.size());
    const std::uint32_t needed = kRegionHeaderSize + 16 * count;
    if (list == 0) {
        write32(sizeAddress, needed);
        return kDdOk;
    }
    if (read32(sizeAddress) < needed) {
        write32(sizeAddress, needed);
        return kDdErrRegionTooSmall;
    }
    RectL bound = count == 0 ? RectL{} : rects[0];
    for (const RectL &rect : rects) {
        bound.left = std::min(bound.left, rect.left);
        bound.top = std::min(bound.top, rect.top);
        bound.right = std::max(bound.right, rect.right);
        bound.bottom = std::max(bound.bottom, rect.bottom);
    }
    const auto writeRect = [](std::uint32_t at, const RectL &rect) {
        write32(at, static_cast<std::uint32_t>(rect.left));
        write32(at + 4, static_cast<std::uint32_t>(rect.top));
        write32(at + 8, static_cast<std::uint32_t>(rect.right));
        write32(at + 12, static_cast<std::uint32_t>(rect.bottom));
    };
    write32(list, kRegionHeaderSize);
    write32(list + 4, kRegionRectangles);
    write32(list + 8, count);
    write32(list + 12, 16 * count);
    writeRect(list + 16, bound); // rcBound
    for (std::uint32_t i = 0; i < count; ++i) {
        writeRect(list + kRegionHeaderSize + 16 * i, rects[i]);
    }
    return kDdOk;
}

std::uint32_t clpGetHWnd() { // (this, hwnd)
    Clipper *self = clipper(A32(0));
    const std::uint32_t out = A32(1);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    std::uint32_t window = 0;
    const HResult result = self->GetHWnd(window);
    write32(out, window);
    return result;
}

std::uint32_t clpInitialize() { // (this, directDraw, flags)
    Clipper *self = clipper(A32(0));
    return self == nullptr ? kDdErrInvalidObject : self->Initialize();
}

std::uint32_t clpIsClipListChanged() { // (this, changed)
    Clipper *self = clipper(A32(0));
    const std::uint32_t out = A32(1);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    bool changed = false;
    const HResult result = self->IsClipListChanged(changed);
    write32(out, changed ? 1u : 0u);
    return result;
}

std::uint32_t clpSetClipList() { // (this, clipList, flags)
    Clipper *self = clipper(A32(0));
    const std::uint32_t list = A32(1);
    if (self == nullptr) {
        return kDdErrInvalidObject;
    }
    if (list == 0) {
        return self->SetClipList(nullptr, A32(2));
    }
    const std::uint32_t count = read32(list + 8);
    constexpr std::uint32_t kMaxClipRects = 65536; // a screen's worth, far more than a game sets
    if (count > kMaxClipRects) {
        return kDdErrInvalidParams;
    }
    std::vector<RectL> rects(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        rects[i] = readRect(list + kRegionHeaderSize + 16 * i);
    }
    return self->SetClipList(&rects, A32(2));
}

std::uint32_t clpSetHWnd() { // (this, flags, hwnd)
    Clipper *self = clipper(A32(0));
    return self == nullptr ? kDdErrInvalidObject : self->SetHWnd(A32(1), A32(2));
}

// ---- the vtables, in slot order, with each row's stack bytes ----

#define SFCOP_IUNKNOWN(iface)                                                                      \
    {"sfcop!" iface "::QueryInterface", shim<queryInterface, 12>},                                 \
        {"sfcop!" iface "::AddRef", shim<addRef, 4>}, {                                            \
        "sfcop!" iface "::Release", shim<release, 4>                                               \
    }

#define SFCOP_DIRECTDRAW(iface, view, setDisplayMode, setDisplayModeBytes)                         \
    SFCOP_IUNKNOWN(iface), {"sfcop!" iface "::Compact", shim<ddCompact, 4>},                       \
        {"sfcop!" iface "::CreateClipper", shim<ddCreateClipper, 16>},                             \
        {"sfcop!" iface "::CreatePalette", shim<ddCreatePalette, 20>},                             \
        {"sfcop!" iface "::CreateSurface", shim<ddCreateSurface<view>, 16>},                       \
        {"sfcop!" iface "::DuplicateSurface", shim<ddDuplicateSurface, 12>},                       \
        {"sfcop!" iface "::EnumDisplayModes", shim<ddEnumDisplayModes<view>, 20>},                 \
        {"sfcop!" iface "::EnumSurfaces", shim<ddEnumSurfaces, 20>},                               \
        {"sfcop!" iface "::FlipToGDISurface", shim<ddFlipToGdiSurface, 4>},                        \
        {"sfcop!" iface "::GetCaps", shim<ddGetCaps, 12>},                                         \
        {"sfcop!" iface "::GetDisplayMode", shim<ddGetDisplayMode, 8>},                            \
        {"sfcop!" iface "::GetFourCCCodes", shim<ddGetFourCcCodes, 12>},                           \
        {"sfcop!" iface "::GetGDISurface", shim<ddGetGdiSurface<view>, 8>},                        \
        {"sfcop!" iface "::GetMonitorFrequency", shim<ddGetMonitorFrequency, 8>},                  \
        {"sfcop!" iface "::GetScanLine", shim<ddGetScanLine, 8>},                                  \
        {"sfcop!" iface "::GetVerticalBlankStatus", shim<ddGetVerticalBlankStatus, 8>},            \
        {"sfcop!" iface "::Initialize", shim<ddInitialize, 8>},                                    \
        {"sfcop!" iface "::RestoreDisplayMode", shim<ddRestoreDisplayMode, 4>},                    \
        {"sfcop!" iface "::SetCooperativeLevel", shim<ddSetCooperativeLevel, 12>},                 \
        {"sfcop!" iface "::SetDisplayMode", shim<setDisplayMode, setDisplayModeBytes>}, {          \
        "sfcop!" iface "::WaitForVerticalBlank", shim<ddWaitForVerticalBlank, 12>                  \
    }

const win32hle_shim kDirectDrawMethods[] = {
    SFCOP_DIRECTDRAW("IDirectDraw", GI::kDirectDraw, ddSetDisplayMode1, 16),
    {nullptr, nullptr},
};

const win32hle_shim kDirectDraw2Methods[] = {
    SFCOP_DIRECTDRAW("IDirectDraw2", GI::kDirectDraw2, ddSetDisplayMode2, 24),
    {"sfcop!IDirectDraw2::GetAvailableVidMem", shim<ddGetAvailableVidMem<GI::kDirectDraw2>, 16>},
    {nullptr, nullptr},
};

const win32hle_shim kDirectDraw4Methods[] = {
    SFCOP_DIRECTDRAW("IDirectDraw4", GI::kDirectDraw4, ddSetDisplayMode2, 24),
    {"sfcop!IDirectDraw4::GetAvailableVidMem", shim<ddGetAvailableVidMem<GI::kDirectDraw4>, 16>},
    {"sfcop!IDirectDraw4::GetSurfaceFromDC", shim<ddGetSurfaceFromDc, 12>},
    {"sfcop!IDirectDraw4::RestoreAllSurfaces", shim<ddRestoreAllSurfaces, 4>},
    {"sfcop!IDirectDraw4::TestCooperativeLevel", shim<ddTestCooperativeLevel, 4>},
    {"sfcop!IDirectDraw4::GetDeviceIdentifier", shim<ddGetDeviceIdentifier, 12>},
    {nullptr, nullptr},
};

#define SFCOP_SURFACE(iface, view)                                                                 \
    SFCOP_IUNKNOWN(iface),                                                                         \
        {"sfcop!" iface "::AddAttachedSurface", shim<srfAddAttachedSurface, 8>},                   \
        {"sfcop!" iface "::AddOverlayDirtyRect", shim<srfUnsupported, 8>},                         \
        {"sfcop!" iface "::Blt", shim<srfBlt, 24>},                                                \
        {"sfcop!" iface "::BltBatch", shim<srfUnsupported, 16>},                                   \
        {"sfcop!" iface "::BltFast", shim<srfBltFast, 24>},                                        \
        {"sfcop!" iface "::DeleteAttachedSurface", shim<srfDeleteAttachedSurface, 12>},            \
        {"sfcop!" iface "::EnumAttachedSurfaces", shim<srfEnumAttachedSurfaces<view>, 12>},        \
        {"sfcop!" iface "::EnumOverlayZOrders", shim<srfOk, 16>},                                  \
        {"sfcop!" iface "::Flip", shim<srfFlip, 12>},                                              \
        {"sfcop!" iface "::GetAttachedSurface", shim<srfGetAttachedSurface<view>, 12>},            \
        {"sfcop!" iface "::GetBltStatus", shim<srfOk, 8>},                                         \
        {"sfcop!" iface "::GetCaps", shim<srfGetCaps<view>, 8>},                                   \
        {"sfcop!" iface "::GetClipper", shim<srfGetClipper, 8>},                                   \
        {"sfcop!" iface "::GetColorKey", shim<srfGetColorKey, 12>},                                \
        {"sfcop!" iface "::GetDC", shim<srfGetDc, 8>},                                             \
        {"sfcop!" iface "::GetFlipStatus", shim<srfOk, 8>},                                        \
        {"sfcop!" iface "::GetOverlayPosition", shim<srfUnsupported, 12>},                         \
        {"sfcop!" iface "::GetPalette", shim<srfGetPalette, 8>},                                   \
        {"sfcop!" iface "::GetPixelFormat", shim<srfGetPixelFormat, 8>},                           \
        {"sfcop!" iface "::GetSurfaceDesc", shim<srfGetSurfaceDesc, 8>},                           \
        {"sfcop!" iface "::Initialize", shim<srfInitialize, 12>},                                  \
        {"sfcop!" iface "::IsLost", shim<srfIsLost, 4>},                                           \
        {"sfcop!" iface "::Lock", shim<srfLock, 20>},                                              \
        {"sfcop!" iface "::ReleaseDC", shim<srfReleaseDc, 8>},                                     \
        {"sfcop!" iface "::Restore", shim<srfRestore, 4>},                                         \
        {"sfcop!" iface "::SetClipper", shim<srfSetClipper, 8>},                                   \
        {"sfcop!" iface "::SetColorKey", shim<srfSetColorKey, 12>},                                \
        {"sfcop!" iface "::SetOverlayPosition", shim<srfUnsupported, 12>},                         \
        {"sfcop!" iface "::SetPalette", shim<srfSetPalette, 8>},                                   \
        {"sfcop!" iface "::Unlock", shim<srfUnlock, 8>},                                           \
        {"sfcop!" iface "::UpdateOverlay", shim<srfUnsupported, 24>},                              \
        {"sfcop!" iface "::UpdateOverlayDisplay", shim<srfUnsupported, 8>},                        \
        {"sfcop!" iface "::UpdateOverlayZOrder", shim<srfUnsupported, 12>},                        \
        {"sfcop!" iface "::GetDDInterface", shim<srfGetDdInterface<view>, 8>},                     \
        {"sfcop!" iface "::PageLock", shim<srfOk, 8>},                                             \
        {"sfcop!" iface "::PageUnlock", shim<srfOk, 8>}, {                                         \
        "sfcop!" iface "::SetSurfaceDesc", shim<srfUnsupported, 12>                                \
    }

const win32hle_shim kSurface3Methods[] = {
    SFCOP_SURFACE("IDirectDrawSurface3", GI::kSurface3),
    {nullptr, nullptr},
};

const win32hle_shim kSurface4Methods[] = {
    SFCOP_SURFACE("IDirectDrawSurface4", GI::kSurface4),
    {"sfcop!IDirectDrawSurface4::SetPrivateData", shim<srfSetPrivateData, 20>},
    {"sfcop!IDirectDrawSurface4::GetPrivateData", shim<srfGetPrivateData, 16>},
    {"sfcop!IDirectDrawSurface4::FreePrivateData", shim<srfFreePrivateData, 8>},
    {"sfcop!IDirectDrawSurface4::GetUniquenessValue", shim<srfGetUniquenessValue, 8>},
    {"sfcop!IDirectDrawSurface4::ChangeUniquenessValue", shim<srfChangeUniquenessValue, 4>},
    {nullptr, nullptr},
};

const win32hle_shim kPaletteMethods[] = {
    SFCOP_IUNKNOWN("IDirectDrawPalette"),
    {"sfcop!IDirectDrawPalette::GetCaps", shim<palGetCaps, 8>},
    {"sfcop!IDirectDrawPalette::GetEntries", shim<palGetEntries, 20>},
    {"sfcop!IDirectDrawPalette::Initialize", shim<palInitialize, 16>},
    {"sfcop!IDirectDrawPalette::SetEntries", shim<palSetEntries, 20>},
    {nullptr, nullptr},
};

const win32hle_shim kClipperMethods[] = {
    SFCOP_IUNKNOWN("IDirectDrawClipper"),
    {"sfcop!IDirectDrawClipper::GetClipList", shim<clpGetClipList, 16>},
    {"sfcop!IDirectDrawClipper::GetHWnd", shim<clpGetHWnd, 8>},
    {"sfcop!IDirectDrawClipper::Initialize", shim<clpInitialize, 12>},
    {"sfcop!IDirectDrawClipper::IsClipListChanged", shim<clpIsClipListChanged, 8>},
    {"sfcop!IDirectDrawClipper::SetClipList", shim<clpSetClipList, 12>},
    {"sfcop!IDirectDrawClipper::SetHWnd", shim<clpSetHWnd, 12>},
    {nullptr, nullptr},
};

#undef SFCOP_SURFACE
#undef SFCOP_DIRECTDRAW
#undef SFCOP_IUNKNOWN

const win32hle_shim *methodsOf(GI iface) {
    switch (iface) {
    case GI::kDirectDraw:
        return kDirectDrawMethods;
    case GI::kDirectDraw2:
        return kDirectDraw2Methods;
    case GI::kDirectDraw4:
        return kDirectDraw4Methods;
    case GI::kSurface3:
        return kSurface3Methods;
    case GI::kSurface4:
        return kSurface4Methods;
    case GI::kPalette:
        return kPaletteMethods;
    default:
        return kClipperMethods;
    }
}

// A view: the interface's vtable (built on first use, once guest memory exists) and a count word
// the shims leave alone (the count is the host object's).
std::uint32_t makeView(GI iface) {
    static std::map<GI, std::uint32_t> vtables;
    std::uint32_t &vtable = vtables[iface];
    if (vtable == 0) {
        vtable = hle_com_vtable(methodsOf(iface));
    }
    return hle_com_new(vtable, 8);
}

void onDestroyed(const ComObject *object) {
    GuestViews::Instance().HostDestroyed(*object);
    for (auto i = deviceContexts().begin(); i != deviceContexts().end();) {
        i = i->second == object ? deviceContexts().erase(i) : std::next(i); // a DC left out
    }
    for (auto i = privateData().begin(); i != privateData().end();) {
        i = i->first.first == object ? privateData().erase(i) : std::next(i);
    }
}

// Installed when the program starts (this file is compiled only into the client).
[[maybe_unused]] const bool kInstalled = [] {
    GuestViews &all = GuestViews::Instance();
    all.SetMaker(GI::kDirectDraw, GI::kClipper, makeView);
    all.SetFreer(hle_com_free);
    ComObject::SetDestroyHook(onDestroyed);
    return true;
}();

// ---- DDRAW.DLL ----

// TEMPORARY: without a guest host (GuestViews.h) the boot platform's DirectDraw answers.
void handOffToBootPlatform(const char *name) {
    for (const win32hle_shim *table : {win32hle_ddraw, sfcop_hle_misc}) {
        for (const win32hle_shim *entry = table; entry->name != nullptr; ++entry) {
            if (std::strcmp(entry->name, name) == 0) {
                entry->fn(); // it reads its arguments and purges the stack itself
                return;
            }
        }
    }
    RET(kDdErrUnsupported, 0); // unreachable while the boot platform serves these three
}

bool haveHost() {
    return GuestGpu() != nullptr && GuestWindow() != nullptr;
}

// DDCREATE_HARDWAREONLY and DDCREATE_EMULATIONONLY: the one display device either way.
constexpr std::uint32_t kLastCreateFlag = 2;

std::uint32_t directDrawCreate() { // (guid, lplpDD, outer)
    const std::uint32_t guid = A32(0);
    const std::uint32_t out = A32(1);
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    write32(out, 0);
    if (A32(2) != 0) {
        return kClassENoAggregation;
    }
    if (guid > kLastCreateFlag) {
        return kDdErrInvalidDirectDrawGuid;
    }
    auto *created = new DirectDraw(*GuestGpu(), *GuestWindow(), pixelMemory());
    return handOut(out, created, GI::kDirectDraw);
}

std::uint32_t directDrawCreateClipper() { // (flags, lplpClipper, outer)
    const std::uint32_t out = A32(1);
    if (out == 0) {
        return kDdErrInvalidParams;
    }
    write32(out, 0);
    if (A32(2) != 0) {
        return kClassENoAggregation;
    }
    return handOut(out, new Clipper(nullptr), GI::kClipper);
}

// The one display device, as the NULL GUID.
std::uint32_t directDrawEnumerateA() { // (callback, context)
    const std::uint32_t callback = A32(0);
    const std::uint32_t context = A32(1);
    if (callback == 0) {
        return kDdErrInvalidParams;
    }
    Scratch scratch;
    callGuest(callback,
              {0, scratch.Text("Primary Display Driver"), scratch.Text("display"), context});
    return kDdOk;
}

void directDrawCreateShim() {
    if (!haveHost()) {
        handOffToBootPlatform("DirectDrawCreate");
        return;
    }
    shim<directDrawCreate, 12>();
}

void directDrawCreateClipperShim() {
    if (!haveHost()) {
        handOffToBootPlatform("DirectDrawCreateClipper");
        return;
    }
    shim<directDrawCreateClipper, 12>();
}

void directDrawEnumerateAShim() {
    if (!haveHost()) {
        handOffToBootPlatform("DirectDrawEnumerateA");
        return;
    }
    shim<directDrawEnumerateA, 8>();
}

} // namespace

} // namespace sfcop::hle::ddraw4

extern "C" const win32hle_shim sfcop_hle_ddraw4[] = {
    {"DirectDrawCreate", sfcop::hle::ddraw4::directDrawCreateShim},
    {"DirectDrawCreateClipper", sfcop::hle::ddraw4::directDrawCreateClipperShim},
    {"DirectDrawEnumerateA", sfcop::hle::ddraw4::directDrawEnumerateAShim},
    {nullptr, nullptr},
};

#endif // __has_include("win32hle.h")
