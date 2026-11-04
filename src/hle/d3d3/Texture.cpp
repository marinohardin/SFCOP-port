#include "hle/d3d3/Texture.h"
#include "hle/ddraw4/Palette.h"
#include "hle/ddraw4/Surface.h"

#include <map>

namespace sfcop::hle::d3d3 {

namespace {

// Texture handles of every device. A handle names a surface and lives as long as it does (in
// Direct3D 3 the texture is the surface; the IDirect3DTexture2 view comes and goes). Handles are
// never reused.
struct Handles {
    std::map<std::uint32_t, ddraw4::Surface *> surfaces;
    std::uint32_t next = 1;
};

Handles &handles() {
    static Handles table;
    return table;
}

// The next mipmap level of `surface`, AddRef'd, or nullptr.
ddraw4::Surface *nextLevel(ddraw4::Surface &surface) {
    ddraw4::Surface *level = nullptr;
    if (surface.GetAttachedSurface(ddraw4::SurfaceCaps{ddraw4::kDdscapsMipMap, 0, 0, 0}, &level) !=
        kDdOk) {
        return nullptr;
    }
    return level;
}

} // namespace

Texture::Texture(ddraw4::Surface &surface) : surface_(surface) {
    surface_.AddRef(); // the reference QueryInterface for this view added (the header)
}

Texture::~Texture() {
    surface_.Release();
}

HResult Texture::GetHandle(const Device *, std::uint32_t &handle) {
    if (surface_.TextureHandle() == 0) {
        const std::uint32_t made = handles().next++;
        surface_.SetTextureHandle(made);
        handles().surfaces[made] = &surface_;
        ddraw4::Surface::SetHandleGoneHook([](std::uint32_t gone) { handles().surfaces.erase(gone); });
    }
    handle = surface_.TextureHandle();
    return kDdOk;
}

HResult Texture::PaletteChanged(std::uint32_t, std::uint32_t) const {
    return kDdOk;
}

HResult Texture::Load(Texture *source) {
    if (source == nullptr) {
        return ddraw4::kDdErrInvalidParams;
    }
    ddraw4::Surface &from = source->surface_;
    if (from.Width() != surface_.Width() || from.Height() != surface_.Height() ||
        from.Format() != surface_.Format()) {
        return ddraw4::kDdErrInvalidParams;
    }
    ddraw4::ColorKey key;
    if (from.GetColorKey(ddraw4::kDdckeySrcBlt, key) == kDdOk) {
        (void)surface_.SetColorKey(ddraw4::kDdckeySrcBlt | ddraw4::kDdckeyColorSpace, &key);
    }
    ddraw4::Palette *fromPalette = from.AttachedPalette();
    ddraw4::Palette *toPalette = surface_.AttachedPalette();
    if (fromPalette != nullptr && toPalette != nullptr && fromPalette != toPalette) {
        const auto &colors = fromPalette->Entries();
        (void)toPalette->SetEntries(0, 0, static_cast<std::uint32_t>(colors.size()), colors.data());
    }

    // Level by level, as long as both chains go on.
    ddraw4::Surface *to = &surface_;
    ddraw4::Surface *of = &from;
    to->AddRef();
    of->AddRef();
    HResult result = kDdOk;
    while (to != nullptr && of != nullptr && result == kDdOk) {
        result = to->Blt(nullptr, of, nullptr, 0, nullptr);
        ddraw4::Surface *nextTo = nextLevel(*to);
        ddraw4::Surface *nextOf = nextLevel(*of);
        to->Release();
        of->Release();
        to = nextTo;
        of = nextOf;
    }
    if (to != nullptr) {
        to->Release();
    }
    if (of != nullptr) {
        of->Release();
    }
    return result;
}

Texture *Texture::FromHandle(std::uint32_t handle) {
    const auto found = handles().surfaces.find(handle);
    return found == handles().surfaces.end() ? nullptr : new Texture(*found->second);
}

} // namespace sfcop::hle::d3d3
