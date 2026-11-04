// The Direct3D module's half of QueryInterface (GuestViews::Query): which view of which object an
// IID names for IDirect3D3, IDirect3DDevice3, IDirect3DViewport3 and IDirect3DTexture2, and the
// objects made on the way (the Direct3D object of a DirectDraw object, the texture view of a
// surface). Plain C++ with no guest memory, so that the decisions are tested.
#pragma once

#include "hle/ddraw4/GuestViews.h"

namespace sfcop::hle::d3d3 {

// The hook the guest shims install with GuestViews::SetQueryHook: the address of a view holding a
// new reference, or 0 for E_NOINTERFACE (nothing changed).
//   - a DirectDraw object (any interface) asked for IDirect3D3: the Direct3D object of that
//     DirectDraw object, made on first use and shared after;
//   - a surface asked for IDirect3DTexture2: the texture view of that surface, likewise;
//   - the Direct3D objects' own interfaces (their IID, and IUnknown for all but the texture);
//   - a texture view asked for IUnknown or an IDirectDrawSurface interface: the surface.
std::uint32_t queryView(ddraw4::ComObject &host, ddraw4::GuestInterface iface,
                        const ddraw4::Guid &iid);

} // namespace sfcop::hle::d3d3
