// QueryInterface's decisions: which view an IID names on which object, the shared counts, and the
// seam between the DirectDraw objects and the Direct3D ones (GuestViews::Query, queryView).
#include "hle/d3d3/D3d3TestRig.h"
#include "hle/d3d3/Views.h"

namespace sfcop::hle::d3d3 {

using ddraw4::ComObject;
using ddraw4::Guid;
using ddraw4::GuestInterface;
using ddraw4::GuestViews;
using GI = GuestInterface;
using test::D3dRig;

namespace {

std::uint32_t g_next = 0x1000;

std::uint32_t makeView(GuestInterface) {
    g_next += 0x10;
    return g_next;
}

void freeView(std::uint32_t) {}

// A fresh registry with fake views and the Direct3D hook installed.
struct Views {
    GuestViews &views = GuestViews::Instance();
    Views() {
        views.Reset();
        g_next = 0x1000;
        views.SetMaker(GI::kDirectDraw, GI::kTexture2, makeView);
        views.SetFreer(freeView);
        views.SetQueryHook(queryView);
    }
    ~Views() { views.Reset(); }

    GuestInterface Shows(std::uint32_t address) const {
        const auto view = views.Find(address);
        REQUIRE(view.has_value());
        return view->iface;
    }
    ComObject *HostOf(std::uint32_t address) const {
        const auto view = views.Find(address);
        REQUIRE(view.has_value());
        return view->host;
    }
};

const Guid kUnknownIid = ddraw4::makeGuid(0x12345678, 0x1234, 0x1234, {1, 2, 3, 4, 5, 6, 7, 8});

} // namespace

TEST_CASE("a DirectDraw object serves its own interfaces and IUnknown") {
    D3dRig rig;
    Views all;
    ComObject &dd = *rig.dd;
    const std::uint32_t before = dd.RefCount();
    struct Case {
        Guid iid;
        GuestInterface shows;
    };
    const Case cases[] = {{ddraw4::kIidUnknown, GI::kDirectDraw},
                          {ddraw4::kIidDirectDraw, GI::kDirectDraw},
                          {ddraw4::kIidDirectDraw2, GI::kDirectDraw2},
                          {ddraw4::kIidDirectDraw4, GI::kDirectDraw4}};
    std::uint32_t expected = before;
    for (const Case &c : cases) {
        const std::uint32_t view = all.views.Query(dd, GI::kDirectDraw4, c.iid);
        REQUIRE(view != 0);
        CHECK(all.Shows(view) == c.shows);
        CHECK(all.HostOf(view) == &dd);
        CHECK(dd.RefCount() == ++expected);
    }
    // The same interface twice is the same view.
    CHECK(all.views.Query(dd, GI::kDirectDraw, ddraw4::kIidDirectDraw4) ==
          all.views.Query(dd, GI::kDirectDraw2, ddraw4::kIidDirectDraw4));
    for (std::uint32_t i = before; i < dd.RefCount();) {
        dd.Release();
    }
}

TEST_CASE("a surface serves IDirectDrawSurface 1 to 3 as one view and 4 as another") {
    D3dRig rig;
    Views all;
    ddraw4::Surface *surface =
        rig.Create(ddraw4::test::plainDesc(8, 8, platform::PixelFormat::kRgb565));
    const std::uint32_t before = surface->RefCount();
    const Guid three[] = {ddraw4::kIidUnknown, ddraw4::kIidDirectDrawSurface,
                          ddraw4::kIidDirectDrawSurface2, ddraw4::kIidDirectDrawSurface3};
    std::uint32_t first = 0;
    for (const Guid &iid : three) {
        const std::uint32_t view = all.views.Query(*surface, GI::kSurface4, iid);
        REQUIRE(view != 0);
        CHECK(all.Shows(view) == GI::kSurface3);
        CHECK((first == 0 || view == first));
        first = view;
    }
    const std::uint32_t four =
        all.views.Query(*surface, GI::kSurface3, ddraw4::kIidDirectDrawSurface4);
    REQUIRE(four != 0);
    CHECK(all.Shows(four) == GI::kSurface4);
    CHECK(four != first);
    CHECK(surface->RefCount() == before + 5);
    // One shared count: five Releases, through whichever views, give the five references back.
    for (int i = 0; i < 5; ++i) {
        all.HostOf(i % 2 == 0 ? first : four)->Release();
    }
    CHECK(surface->RefCount() == before);
    surface->Release();
}

TEST_CASE("an interface the object does not serve is E_NOINTERFACE and changes no count") {
    D3dRig rig;
    Views all;
    ddraw4::Surface *surface =
        rig.Create(ddraw4::test::plainDesc(8, 8, platform::PixelFormat::kRgb565));
    const std::uint32_t surfaceCount = surface->RefCount();
    const std::uint32_t ddCount = rig.dd->RefCount();
    CHECK(all.views.Query(*surface, GI::kSurface3, kUnknownIid) == 0);
    CHECK(all.views.Query(*surface, GI::kSurface3, ddraw4::kIidDirectDraw4) == 0);
    CHECK(all.views.Query(*surface, GI::kSurface3, ddraw4::kIidDirectDrawPalette) == 0);
    CHECK(all.views.Query(*rig.dd, GI::kDirectDraw4, kUnknownIid) == 0);
    CHECK(all.views.Query(*rig.dd, GI::kDirectDraw4, ddraw4::kIidDirectDrawSurface4) == 0);
    CHECK(all.views.Query(*rig.dd, GI::kDirectDraw4, kIidDirect3DTexture2) == 0);
    CHECK(surface->RefCount() == surfaceCount);
    CHECK(rig.dd->RefCount() == ddCount);
    CHECK(all.views.Size() == 0);
    surface->Release();
}

TEST_CASE("without a hook the Direct3D interfaces are not served") {
    D3dRig rig;
    Views all;
    all.views.SetQueryHook(nullptr);
    CHECK(all.views.Query(*rig.dd, GI::kDirectDraw4, kIidDirect3D3) == 0);
}

TEST_CASE("IDirect3D3 comes from a DirectDraw object, one Direct3D object per DirectDraw") {
    D3dRig rig;
    Views all;
    const std::uint32_t ddCount = rig.dd->RefCount();
    const std::uint32_t first = all.views.Query(*rig.dd, GI::kDirectDraw4, kIidDirect3D3);
    REQUIRE(first != 0);
    CHECK(all.Shows(first) == GI::kDirect3D3);
    ComObject *d3d = all.HostOf(first);
    CHECK(d3d->RefCount() == 1);
    CHECK(rig.dd->RefCount() == ddCount); // the Direct3D object keeps its DirectDraw object alive
                                         // without a count the game can see

    // From another DirectDraw interface: the same Direct3D object, one more reference.
    const std::uint32_t again = all.views.Query(*rig.dd, GI::kDirectDraw, kIidDirect3D3);
    CHECK(again == first);
    CHECK(d3d->RefCount() == 2);
    // IUnknown and its own IID on the Direct3D view add a reference to the same object.
    CHECK(all.views.Query(*d3d, GI::kDirect3D3, ddraw4::kIidUnknown) == first);
    CHECK(all.views.Query(*d3d, GI::kDirect3D3, kIidDirect3D3) == first);
    CHECK(d3d->RefCount() == 4);
    // It does not serve the device or a surface interface.
    CHECK(all.views.Query(*d3d, GI::kDirect3D3, kIidDirect3DDevice3) == 0);
    CHECK(all.views.Query(*d3d, GI::kDirect3D3, ddraw4::kIidDirectDrawSurface4) == 0);
    CHECK(d3d->RefCount() == 4);
    for (int i = 0; i < 4; ++i) {
        d3d->Release();
    }
}

TEST_CASE("IDirect3DTexture2 comes from a surface and gives the surface back") {
    D3dRig rig;
    Views all;
    ddraw4::Surface *surface =
        rig.Create(ddraw4::test::plainDesc(16, 16, platform::PixelFormat::kRgb565));
    for (GuestInterface asked : {GI::kSurface3, GI::kSurface4}) {
        CHECK(all.views.Query(*surface, asked, ddraw4::kIidDirectDrawSurface4) != 0);
        surface->Release();
    }
    REQUIRE(surface->RefCount() == 1);

    const std::uint32_t view = all.views.Query(*surface, GI::kSurface3, kIidDirect3DTexture2);
    REQUIRE(view != 0);
    CHECK(all.Shows(view) == GI::kTexture2);
    ComObject *texture = all.HostOf(view);
    CHECK(texture->RefCount() == 1);
    CHECK(surface->RefCount() == 2); // the view holds one on its surface

    // Again, from the other surface interface: the same view, one more on the texture.
    CHECK(all.views.Query(*surface, GI::kSurface4, kIidDirect3DTexture2) == view);
    CHECK(texture->RefCount() == 2);
    CHECK(surface->RefCount() == 2);
    // The texture's own IID adds to the texture; IUnknown is the surface's.
    CHECK(all.views.Query(*texture, GI::kTexture2, kIidDirect3DTexture2) == view);
    CHECK(texture->RefCount() == 3);
    const std::uint32_t asSurface = all.views.Query(*texture, GI::kTexture2, ddraw4::kIidUnknown);
    REQUIRE(asSurface != 0);
    CHECK(all.Shows(asSurface) == GI::kSurface3);
    CHECK(all.HostOf(asSurface) == surface);
    CHECK(surface->RefCount() == 3);
    const std::uint32_t asSurface4 =
        all.views.Query(*texture, GI::kTexture2, ddraw4::kIidDirectDrawSurface4);
    CHECK(all.Shows(asSurface4) == GI::kSurface4);
    CHECK(surface->RefCount() == 4);
    CHECK(all.views.Query(*texture, GI::kTexture2, kIidDirect3DDevice3) == 0);
    CHECK(all.views.Query(*texture, GI::kTexture2, kUnknownIid) == 0);
    CHECK(texture->RefCount() == 3);
    CHECK(surface->RefCount() == 4);

    // Releasing through the surface views and the texture view each balance their own object.
    surface->Release();
    surface->Release();
    for (int i = 0; i < 3; ++i) {
        texture->Release();
    }
    CHECK(surface->RefCount() == 1);
    surface->Release();
}

TEST_CASE("the Direct3D objects serve their own interfaces") {
    D3dRig rig;
    Views all;
    Device *device = rig.MakeDevice();
    Viewport *viewport = rig.MakeViewport(*device);
    const std::uint32_t deviceView = all.views.View(device, GI::kDevice3);
    const std::uint32_t viewportView = all.views.View(viewport, GI::kViewport3);
    REQUIRE(deviceView != 0);
    REQUIRE(viewportView != 0);
    const std::uint32_t deviceCount = device->RefCount();
    const std::uint32_t viewportCount = viewport->RefCount();
    CHECK(all.views.Query(*device, GI::kDevice3, kIidDirect3DDevice3) == deviceView);
    CHECK(all.views.Query(*device, GI::kDevice3, ddraw4::kIidUnknown) == deviceView);
    CHECK(all.views.Query(*device, GI::kDevice3, kIidDirect3DViewport3) == 0);
    CHECK(device->RefCount() == deviceCount + 2);
    CHECK(all.views.Query(*viewport, GI::kViewport3, kIidDirect3DViewport3) == viewportView);
    CHECK(all.views.Query(*viewport, GI::kViewport3, ddraw4::kIidUnknown) == viewportView);
    CHECK(all.views.Query(*viewport, GI::kViewport3, kIidDirect3DDevice3) == 0);
    CHECK(viewport->RefCount() == viewportCount + 2);
    device->Release();
    device->Release();
    viewport->Release();
    viewport->Release();
    viewport->Release();
    device->Release();
}

} // namespace sfcop::hle::d3d3
