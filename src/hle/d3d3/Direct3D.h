// The Direct3D object behind IDirect3D3 (DESIGN §5.6, §6.1). The game obtains it by
// QueryInterface on its DirectDraw object; the guest layer makes one per DirectDraw object and
// hands it out again on later queries. It keeps that DirectDraw object alive (ComObject.h) and
// offers one device: a hardware rasteriser on the Gpu, which draws the game's transformed and lit
// vertices.
#pragma once

#include "hle/d3d3/D3d3Defs.h"
#include "hle/ddraw4/ComObject.h"

#include <vector>

namespace sfcop::hle::ddraw4 {
class DirectDraw;
class Surface;
} // namespace sfcop::hle::ddraw4

namespace sfcop::hle::d3d3 {

class Device;
class Viewport;

class Direct3D final : public ddraw4::ComObject {
public:
    explicit Direct3D(ddraw4::DirectDraw &owner);

    // The devices offered: the one hardware device.
    HResult EnumDevices(std::vector<DeviceInfo> &out) const;

    // The hardware device, unless the search names another GUID or asks for software only
    // (DDERR_NOTFOUND).
    HResult FindDevice(const DeviceSearch &search, DeviceInfo &out) const;

    // A device drawing into `target`, which must be a 3D-capable surface. The hardware and RGB
    // device GUIDs both give the one device; any other is DDERR_INVALIDPARAMS.
    HResult CreateDevice(const Guid &device, ddraw4::Surface *target, Device **out);

    HResult CreateViewport(Viewport **out);

    // The z-buffer formats of a device: 16 bits only.
    HResult EnumZBufferFormats(const Guid &device, std::vector<ddraw4::PixelFormatDesc> &out) const;

    HResult EvictManagedTextures() { return kDdOk; }
    HResult Initialize() const { return ddraw4::kDdErrAlreadyInitialized; }

    // The hardware device's description, sized by the Gpu's caps.
    DeviceInfo HalDevice() const;

    ddraw4::DirectDraw &Owner() const { return owner_; }

private:
    ~Direct3D() override;

    ddraw4::DirectDraw &owner_;
};

} // namespace sfcop::hle::d3d3
