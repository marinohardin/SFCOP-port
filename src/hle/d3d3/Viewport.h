// IDirect3DViewport3: a rectangle of the device's target and the clip volume that untransformed
// vertices map into it. Its clears are the device's (Device::Clear). Lights and the background
// material are kept but not applied: the game draws lit vertices and clears with Clear2.
#pragma once

#include "hle/d3d3/D3d3Defs.h"
#include "hle/ddraw4/ComObject.h"

#include <optional>
#include <vector>

namespace sfcop::hle::d3d3 {

class Device;
class Direct3D;

// D3DVIEWPORT (version 1) without its dwSize.
struct ViewportV1 {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    float scaleX = 0.0f;
    float scaleY = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
    float minZ = 0.0f;
    float maxZ = 1.0f;
};

class Viewport final : public ddraw4::ComObject {
public:
    explicit Viewport(Direct3D &owner);

    HResult SetViewport2(const ViewportDesc &desc);
    // D3DERR_VIEWPORTDATANOTSET before a Set call.
    HResult GetViewport2(ViewportDesc &out) const;

    // The version 1 form: its clip extents (maxX, maxY) become a clip volume of
    // (-maxX, maxY, 2 maxX, 2 maxY). The scale is what the extents imply for the size.
    HResult SetViewport(const ViewportV1 &desc);
    HResult GetViewport(ViewportV1 &out) const;

    // Clears with the background colour (black: no material is applied) and depth 1.
    // D3DERR_VIEWPORTHASNODEVICE before AddViewport.
    HResult Clear(const std::vector<D3dRect> &rects, std::uint32_t flags);
    HResult Clear2(const std::vector<D3dRect> &rects, std::uint32_t flags, std::uint32_t color,
                   float z, std::uint32_t stencil);

    HResult SetBackground(std::uint32_t material);
    HResult GetBackground(std::uint32_t &material, bool &valid) const;

    HResult Initialize() const { return ddraw4::kDdErrAlreadyInitialized; }

    // The viewport's data, if set.
    const std::optional<ViewportDesc> &Desc() const { return desc_; }

    Device *AttachedDevice() const { return device_; }
    void AttachDevice(Device *device) { device_ = device; }

private:
    ~Viewport() override;

    Direct3D &owner_;
    Device *device_ = nullptr; // set while the viewport is in a device's list
    std::optional<ViewportDesc> desc_;
    std::uint32_t background_ = 0;
};

} // namespace sfcop::hle::d3d3
