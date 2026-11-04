#include "hle/d3d3/Viewport.h"
#include "hle/d3d3/Device.h"
#include "hle/d3d3/Direct3D.h"

namespace sfcop::hle::d3d3 {

Viewport::Viewport(Direct3D &owner) : owner_(owner) {
    owner_.AdoptChild();
}

Viewport::~Viewport() {
    owner_.ChildGone();
}

HResult Viewport::SetViewport2(const ViewportDesc &desc) {
    if (desc.clipWidth == 0.0f || desc.clipHeight == 0.0f) {
        return ddraw4::kDdErrInvalidParams;
    }
    desc_ = desc;
    return kDdOk;
}

HResult Viewport::GetViewport2(ViewportDesc &out) const {
    if (!desc_) {
        return kD3dErrViewportDataNotSet;
    }
    out = *desc_;
    return kDdOk;
}

HResult Viewport::SetViewport(const ViewportV1 &desc) {
    if (desc.maxX == 0.0f || desc.maxY == 0.0f) {
        return ddraw4::kDdErrInvalidParams;
    }
    ViewportDesc converted;
    converted.x = desc.x;
    converted.y = desc.y;
    converted.width = desc.width;
    converted.height = desc.height;
    converted.clipX = -desc.maxX;
    converted.clipY = desc.maxY;
    converted.clipWidth = 2.0f * desc.maxX;
    converted.clipHeight = 2.0f * desc.maxY;
    converted.minZ = desc.minZ;
    converted.maxZ = desc.maxZ;
    desc_ = converted;
    return kDdOk;
}

HResult Viewport::GetViewport(ViewportV1 &out) const {
    if (!desc_) {
        return kD3dErrViewportDataNotSet;
    }
    const ViewportDesc &desc = *desc_;
    out = ViewportV1{};
    out.x = desc.x;
    out.y = desc.y;
    out.width = desc.width;
    out.height = desc.height;
    out.maxX = desc.clipWidth / 2.0f;
    out.maxY = desc.clipHeight / 2.0f;
    out.scaleX = static_cast<float>(desc.width) / desc.clipWidth;
    out.scaleY = static_cast<float>(desc.height) / desc.clipHeight;
    out.minZ = desc.minZ;
    out.maxZ = desc.maxZ;
    return kDdOk;
}

HResult Viewport::Clear(const std::vector<D3dRect> &rects, std::uint32_t flags) {
    return Clear2(rects, flags, 0xFF000000, 1.0f, 0);
}

HResult Viewport::Clear2(const std::vector<D3dRect> &rects, std::uint32_t flags,
                         std::uint32_t color, float z, std::uint32_t) {
    if (device_ == nullptr) {
        return kD3dErrViewportHasNoDevice;
    }
    return device_->Clear(*this, rects, flags, color, z);
}

HResult Viewport::SetBackground(std::uint32_t material) {
    background_ = material;
    return kDdOk;
}

HResult Viewport::GetBackground(std::uint32_t &material, bool &valid) const {
    material = background_;
    valid = background_ != 0;
    return kDdOk;
}

} // namespace sfcop::hle::d3d3
