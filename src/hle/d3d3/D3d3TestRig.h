// What the Direct3D doctests share (tests only): the game's display as it sets it up before
// creating its device (800x600 at 16 bits, a flipping primary whose back buffer has a z-buffer
// attached) and the Direct3D object on top of it.
#pragma once

#include "hle/d3d3/Device.h"
#include "hle/d3d3/Direct3D.h"
#include "hle/d3d3/Texture.h"
#include "hle/d3d3/Viewport.h"
#include "hle/ddraw4/Ddraw4TestRig.h"

namespace sfcop::hle::d3d3::test {

using ddraw4::test::Calls;

// The transformed, lit vertex the game draws.
struct TlVertex {
    float x, y, z, rhw;
    std::uint32_t diffuse;
    std::uint32_t specular;
    float u, v;
};

struct D3dRig : ddraw4::test::Rig {
    ddraw4::Surface *front = nullptr;
    ddraw4::Surface *back = nullptr;
    ddraw4::Surface *z = nullptr;
    Direct3D *d3d = nullptr;

    D3dRig() {
        REQUIRE(dd->SetDisplayMode(800, 600, 16, 0, 0) == kDdOk);
        front = Create(ddraw4::test::flipChainDesc());
        REQUIRE(front->GetAttachedSurface(ddraw4::SurfaceCaps{ddraw4::kDdscapsBackBuffer, 0, 0, 0},
                                          &back) == kDdOk);
        ddraw4::SurfaceDesc zDesc;
        zDesc.flags =
            ddraw4::kDdsdCaps | ddraw4::kDdsdWidth | ddraw4::kDdsdHeight | ddraw4::kDdsdPixelFormat;
        zDesc.width = 800;
        zDesc.height = 600;
        zDesc.caps.caps = ddraw4::kDdscapsZBuffer;
        zDesc.pixelFormat = ddraw4::describe(platform::PixelFormat::kDepth16);
        z = Create(zDesc);
        REQUIRE(back->AddAttachedSurface(z) == kDdOk);
        d3d = new Direct3D(*dd); // the reference QueryInterface hands out
        gpu.log.Clear();
        window.log.Clear();
    }

    Device *MakeDevice() {
        Device *device = nullptr;
        REQUIRE(d3d->CreateDevice(kIidHalDevice, back, &device) == kDdOk);
        REQUIRE(device != nullptr);
        return device;
    }

    // A viewport over the whole back buffer, added to `device` and made current.
    Viewport *MakeViewport(Device &device) {
        Viewport *viewport = nullptr;
        REQUIRE(d3d->CreateViewport(&viewport) == kDdOk);
        ViewportDesc desc;
        desc.width = 800;
        desc.height = 600;
        REQUIRE(viewport->SetViewport2(desc) == kDdOk);
        REQUIRE(device.AddViewport(viewport) == kDdOk);
        REQUIRE(device.SetCurrentViewport(viewport) == kDdOk);
        return viewport;
    }
};

} // namespace sfcop::hle::d3d3::test
