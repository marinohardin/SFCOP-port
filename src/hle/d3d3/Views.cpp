#include "hle/d3d3/Views.h"

#include "hle/d3d3/D3d3Defs.h"
#include "hle/d3d3/Direct3D.h"
#include "hle/d3d3/Texture.h"
#include "hle/ddraw4/DirectDraw.h"
#include "hle/ddraw4/Surface.h"

namespace sfcop::hle::d3d3 {

namespace {

using ddraw4::ComObject;
using ddraw4::GuestViews;
using GI = ddraw4::GuestInterface;

GuestViews &views() {
    return GuestViews::Instance();
}

// The Direct3D interface `iid` names on an object shown as `iface`, if it serves it itself.
std::optional<GI> ownInterface(GI iface, const ddraw4::Guid &iid) {
    switch (iface) {
    case GI::kDirect3D3:
        if (iid == ddraw4::kIidUnknown || iid == kIidDirect3D3) {
            return iface;
        }
        break;
    case GI::kDevice3:
        if (iid == ddraw4::kIidUnknown || iid == kIidDirect3DDevice3) {
            return iface;
        }
        break;
    case GI::kViewport3:
        if (iid == ddraw4::kIidUnknown || iid == kIidDirect3DViewport3) {
            return iface;
        }
        break;
    case GI::kTexture2:
        if (iid == kIidDirect3DTexture2) {
            return iface;
        }
        break;
    default:
        break;
    }
    return std::nullopt;
}

} // namespace

std::uint32_t queryView(ComObject &host, GI iface, const ddraw4::Guid &iid) {
    if (const auto own = ownInterface(iface, iid)) {
        const std::uint32_t view = views().View(&host, *own);
        if (view != 0) {
            host.AddRef();
        }
        return view;
    }
    if (iface <= GI::kDirectDraw4 && iid == kIidDirect3D3) {
        auto &owner = static_cast<ddraw4::DirectDraw &>(host);
        for (ComObject *existing : views().Hosts(GI::kDirect3D3)) {
            if (&static_cast<Direct3D *>(existing)->Owner() == &owner) {
                const std::uint32_t view = views().View(existing, GI::kDirect3D3);
                if (view != 0) {
                    existing->AddRef();
                }
                return view;
            }
        }
        auto *created = new Direct3D(owner);
        const std::uint32_t view = views().View(created, GI::kDirect3D3);
        if (view == 0) {
            created->Release();
        }
        return view;
    }
    if ((iface == GI::kSurface3 || iface == GI::kSurface4) && iid == kIidDirect3DTexture2) {
        auto &owner = static_cast<ddraw4::Surface &>(host);
        for (ComObject *existing : views().Hosts(GI::kTexture2)) {
            if (&static_cast<Texture *>(existing)->Surface() == &owner) {
                const std::uint32_t view = views().View(existing, GI::kTexture2);
                if (view != 0) {
                    existing->AddRef();
                }
                return view;
            }
        }
        auto *created = new Texture(owner);
        const std::uint32_t view = views().View(created, GI::kTexture2);
        if (view == 0) {
            created->Release();
        }
        return view;
    }
    if (iface == GI::kTexture2) {
        // The texture is a view of its surface: the surface's interfaces (IUnknown among them).
        ddraw4::Surface &owner = static_cast<Texture &>(host).Surface();
        const GI surfaceView = iid == ddraw4::kIidDirectDrawSurface4 ? GI::kSurface4 : GI::kSurface3;
        if (iid == ddraw4::kIidUnknown || iid == ddraw4::kIidDirectDrawSurface ||
            iid == ddraw4::kIidDirectDrawSurface2 || iid == ddraw4::kIidDirectDrawSurface3 ||
            iid == ddraw4::kIidDirectDrawSurface4) {
            const std::uint32_t view = views().View(&owner, surfaceView);
            if (view != 0) {
                owner.AddRef();
            }
            return view;
        }
    }
    return 0;
}

} // namespace sfcop::hle::d3d3
