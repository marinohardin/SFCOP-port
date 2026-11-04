// IDirect3DTexture2: the texture view of a DirectDraw surface, obtained by QueryInterface on it.
// It holds one reference on its surface, the one that query added, so the game's totals are as
// with one shared count; the guest layer makes one per surface. The pixels, colour key and
// palette are the surface's (Surface.h); this object adds the legacy texture handle and Load.
#pragma once

#include "hle/d3d3/D3d3Defs.h"
#include "hle/ddraw4/ComObject.h"

namespace sfcop::hle::ddraw4 {
class Surface;
} // namespace sfcop::hle::ddraw4

namespace sfcop::hle::d3d3 {

class Device;

class Texture final : public ddraw4::ComObject {
public:
    explicit Texture(ddraw4::Surface &surface);

    // A non-zero number naming this texture's surface for the TEXTUREHANDLE render state, the
    // same for every device and every view of the surface.
    HResult GetHandle(const Device *device, std::uint32_t &handle);

    // Palette changes reach the Gpu when made (Palette.h): nothing to do.
    HResult PaletteChanged(std::uint32_t start, std::uint32_t count) const;

    // Copies `source`'s pixels, mipmap levels, colour key and palette colours into this
    // texture. The sizes and formats must match (DDERR_INVALIDPARAMS).
    HResult Load(Texture *source);

    ddraw4::Surface &Surface() const { return surface_; }

    // A new view (one reference, the caller's) of the surface a handle names, or nullptr when
    // the handle is 0, unknown or its surface is gone. The handle lives as long as the surface,
    // not as long as any view.
    static Texture *FromHandle(std::uint32_t handle);

private:
    ~Texture() override;

    ddraw4::Surface &surface_;
};

} // namespace sfcop::hle::d3d3
