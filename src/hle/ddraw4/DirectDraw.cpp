#include "hle/ddraw4/DirectDraw.h"
#include "hle/ddraw4/Clipper.h"
#include "hle/ddraw4/Palette.h"
#include "hle/ddraw4/Surface.h"

#include "fixes/fixes.h"

#include <algorithm>
#include <map>
#include <utility>

namespace sfcop::hle::ddraw4 {

using platform::Error;
using platform::PixelFormat;

namespace {

constexpr std::uint32_t kVideoMemoryTotal = 256u << 20;
constexpr std::uint32_t kVideoMemoryFree = 192u << 20;

constexpr std::uint32_t kMaxBackBuffers = 3;
constexpr std::uint32_t kDdwaitvbBlockBeginEvent = 0x00000002;

DirectDraw::PresentHook g_presentHook = nullptr;

// The DirectDraw object that made each Gpu's back buffer last. A Gpu has one back buffer, so the
// owner is per Gpu: objects on other Gpus never see each other.
std::map<const platform::Gpu *, const DirectDraw *> &displayOwners() {
    static std::map<const platform::Gpu *, const DirectDraw *> owners;
    return owners;
}

const DirectDraw *displayOwner(const platform::Gpu &gpu) {
    const auto found = displayOwners().find(&gpu);
    return found == displayOwners().end() ? nullptr : found->second;
}

// The resolutions the original's display drivers offered on every card, which the game's options
// expect to find.
constexpr std::pair<std::uint32_t, std::uint32_t> kClassicModes[] = {
    {640, 480}, {800, 600}, {1024, 768}, {1152, 864}, {1280, 960}, {1280, 1024}, {1600, 1200},
};

// Sets the bit for a raster operation in DDCAPS.dwRops: the operation's index is its byte 2.
void addRop(DdCaps &caps, std::uint32_t rop) {
    const std::uint32_t index = (rop >> 16) & 0xFF;
    caps.rops[index / 32] |= 1u << (index % 32);
}

HResult windowError(Error error) {
    return error == Error::kInvalidArgument ? kDdErrInvalidMode : kDdErrGeneric;
}

} // namespace

DirectDraw::DirectDraw(platform::Gpu &gpu, platform::Window &window, PixelMemory &memory)
    : gpu_(gpu), window_(window), memory_(memory) {}

HResult DirectDraw::SetCooperativeLevel(std::uint32_t window, std::uint32_t flags) {
    const bool fullscreen = (flags & kDdsclFullscreen) != 0;
    const bool exclusive = (flags & kDdsclExclusive) != 0;
    const bool normal = (flags & kDdsclNormal) != 0;
    if (fullscreen != exclusive || normal == exclusive || (exclusive && window == 0)) {
        return kDdErrInvalidParams;
    }
    cooperativeWindow_ = window;
    cooperativeFlags_ = flags;
    return kDdOk;
}

HResult DirectDraw::SetDisplayMode(std::uint32_t width, std::uint32_t height,
                                   std::uint32_t bitsPerPixel, std::uint32_t refreshRate,
                                   std::uint32_t) {
    const std::optional<PixelFormat> format = displayFormat(bitsPerPixel);
    if (!format || width == 0 || height == 0) {
        return kDdErrInvalidMode;
    }
    platform::Result<platform::WindowState> state = window_.GetState();
    if (!state.Ok()) {
        return windowError(state.Code());
    }
    const platform::Status resized = window_.SetMode(state.Value().mode, width, height);
    if (!resized.Ok()) {
        return windowError(resized.Code());
    }
    const PixelFormat backFormat = bitsPerPixel == 8 ? PixelFormat::kXrgb8888 : *format;
    platform::Result<platform::TextureId> back =
        gpu_.ConfigureBackBuffer(width, height, backFormat);
    displayTexture_ = {};
    if (!back.Ok()) {
        return back.Code() == Error::kInvalidArgument ? kDdErrInvalidMode : fromError(back.Code());
    }
    displayTexture_ = back.Value();
    displayOwners()[&gpu_] = this;
    mode_ = DisplayModeState{width, height, bitsPerPixel, refreshRate != 0 ? refreshRate : 60};
    return kDdOk;
}

HResult DirectDraw::GetDisplayMode(SurfaceDesc &out) const {
    out = SurfaceDesc{};
    out.flags = kDdsdHeight | kDdsdWidth | kDdsdPitch | kDdsdPixelFormat | kDdsdRefreshRate;
    out.width = mode_.width;
    out.height = mode_.height;
    out.pitch = static_cast<std::int32_t>(mode_.width * mode_.bitsPerPixel / 8);
    out.mipMapCount = mode_.refreshRate;
    out.pixelFormat = describe(*displayFormat(mode_.bitsPerPixel));
    return kDdOk;
}

HResult DirectDraw::EnumDisplayModes(std::uint32_t, const SurfaceDesc *filter,
                                     std::vector<SurfaceDesc> &out) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> sizes(std::begin(kClassicModes),
                                                               std::end(kClassicModes));
    platform::Result<std::vector<platform::DisplayMode>> listed = window_.ListDisplayModes();
    if (listed.Ok()) {
        for (const platform::DisplayMode &mode : listed.Value()) {
            sizes.emplace_back(mode.width, mode.height);
        }
    }
    platform::Result<platform::DisplayMode> desktop = window_.GetDesktopMode();
    if (desktop.Ok()) {
        const platform::DisplayMode limit = desktop.Value();
        sizes.erase(std::remove_if(sizes.begin(), sizes.end(),
                                   [&](const auto &size) {
                                       return size.first > limit.width ||
                                              size.second > limit.height;
                                   }),
                    sizes.end());
    }
    std::sort(sizes.begin(), sizes.end());
    sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());

    out.clear();
    for (std::uint32_t bits : {8u, 16u, 32u}) {
        if (filter != nullptr && (filter->flags & kDdsdPixelFormat) != 0 &&
            filter->pixelFormat.bitCount != bits) {
            continue;
        }
        for (const auto &[width, height] : sizes) {
            if (filter != nullptr &&
                (((filter->flags & kDdsdWidth) != 0 && filter->width != width) ||
                 ((filter->flags & kDdsdHeight) != 0 && filter->height != height))) {
                continue;
            }
            SurfaceDesc mode;
            mode.flags =
                kDdsdHeight | kDdsdWidth | kDdsdPitch | kDdsdPixelFormat | kDdsdRefreshRate;
            mode.width = width;
            mode.height = height;
            mode.pitch = static_cast<std::int32_t>(width * bits / 8);
            mode.mipMapCount = 60;
            mode.pixelFormat = describe(*displayFormat(bits));
            out.push_back(mode);
        }
    }
    return kDdOk;
}

HResult DirectDraw::GetCaps(DdCaps *driver, DdCaps *emulation) const {
    if (driver == nullptr && emulation == nullptr) {
        return kDdErrInvalidParams;
    }
    DdCaps caps;
    caps.caps = kDdcaps3d | kDdcapsBlt | kDdcapsBltStretch | kDdcapsPalette | kDdcapsColorKey |
                kDdcapsBltColorFill;
    caps.caps2 = kDdcaps2WideSurfaces | kDdcaps2CanRenderWindowed;
    caps.colorKeyCaps = kDdckeycapsSrcBlt;
    caps.fxCaps = kDdfxcapsBltStretchAndShrink;
    caps.paletteCaps = kDdpcaps8Bit | kDdpcapsPrimarySurface | kDdpcapsAllow256;
    caps.zBufferBitDepths = kDdbd16;
    caps.vidMemTotal = kVideoMemoryTotal;
    caps.vidMemFree = kVideoMemoryFree;
    addRop(caps, kRopSrcCopy);
    addRop(caps, kRopBlackness);
    addRop(caps, kRopWhiteness);
    caps.surfaceCaps.caps = kDdscapsBackBuffer | kDdscapsComplex | kDdscapsFlip |
                            kDdscapsFrontBuffer | kDdscapsOffscreenPlain | kDdscapsPalette |
                            kDdscapsPrimarySurface | kDdscapsSystemMemory | kDdscapsTexture |
                            kDdscaps3dDevice | kDdscapsVideoMemory | kDdscapsVisible |
                            kDdscapsZBuffer | kDdscapsMipMap | kDdscapsLocalVidMem;
    if (driver != nullptr) {
        *driver = caps;
    }
    if (emulation != nullptr) {
        *emulation = caps;
    }
    return kDdOk;
}

HResult DirectDraw::CreateSurface(const SurfaceDesc &request, Surface **out) {
    if (out == nullptr) {
        return kDdErrInvalidParams;
    }
    *out = nullptr;
    SurfaceDesc desc = request;
    if ((desc.flags & kDdsdCaps) == 0) {
        desc.caps = SurfaceCaps{};
    }
    const std::uint32_t caps = desc.caps.caps;
    if ((desc.flags & kDdsdLpSurface) != 0 || (caps & kDdscapsOverlay) != 0) {
        return kDdErrUnsupported;
    }
    if ((caps & kDdscapsPrimarySurface) != 0) {
        return createPrimary(desc, out);
    }
    if ((desc.flags & kDdsdWidth) == 0 || (desc.flags & kDdsdHeight) == 0 || desc.width == 0 ||
        desc.height == 0) {
        return kDdErrInvalidParams;
    }

    SurfaceRole role = (caps & kDdscapsTexture) != 0 ? SurfaceRole::kTexture : SurfaceRole::kPlain;
    std::optional<PixelFormat> format;
    if ((caps & kDdscapsZBuffer) != 0) {
        role = SurfaceRole::kZBuffer;
        std::uint32_t bits = 0;
        if ((desc.flags & kDdsdPixelFormat) != 0) {
            bits = desc.pixelFormat.bitCount;
        } else if ((desc.flags & kDdsdZBufferBitDepth) != 0) {
            bits = desc.mipMapCount; // dwZBufferBitDepth shares the union with dwMipMapCount
        }
        if (bits == 16) {
            format = PixelFormat::kDepth16;
        }
    } else if ((desc.flags & kDdsdPixelFormat) != 0) {
        format = formatOf(desc.pixelFormat);
        if (format == PixelFormat::kDepth16) {
            format.reset();
        }
    } else {
        format = PrimaryFormat();
    }
    if (!format) {
        return kDdErrInvalidPixelFormat;
    }

    std::uint32_t levels = 1;
    if ((caps & kDdscapsMipMap) != 0 && role == SurfaceRole::kTexture) {
        std::uint32_t possible = 1;
        while ((desc.width >> possible) != 0 && (desc.height >> possible) != 0) {
            ++possible;
        }
        levels = (desc.flags & kDdsdMipMapCount) != 0 ? desc.mipMapCount : possible;
        if (levels == 0 || levels > possible) {
            return kDdErrInvalidParams;
        }
    }

    // The game's reference; the levels below the top are the top's (ComObject.h).
    Surface *top = new Surface(*this, role, desc, *format, nullptr, 0);
    if (!top->Valid()) {
        top->Release();
        return kDdErrOutOfMemory;
    }
    Surface *previous = top;
    for (std::uint32_t level = 1; level < levels; ++level) {
        SurfaceDesc levelDesc = desc;
        levelDesc.width = desc.width >> level;
        levelDesc.height = desc.height >> level;
        Surface *next =
            new Surface(*this, role, levelDesc, *format, top, static_cast<std::uint8_t>(level));
        top->chain_.push_back(next);
        previous->next_ = next;
        previous = next;
        if (!next->Valid()) {
            top->Release();
            return kDdErrOutOfMemory;
        }
    }
    *out = top;
    return kDdOk;
}

HResult DirectDraw::createPrimary(const SurfaceDesc &request, Surface **out) {
    SurfaceDesc desc = request;
    const std::uint32_t caps = desc.caps.caps;
    std::uint32_t backBuffers = 0;
    if ((caps & kDdscapsFlip) != 0) {
        backBuffers = (desc.flags & kDdsdBackBufferCount) != 0 ? desc.backBufferCount : 0;
        if (backBuffers == 0) {
            return kDdErrInvalidCaps;
        }
        if (backBuffers > kMaxBackBuffers) {
            return kDdErrInvalidParams;
        }
    }
    desc.width = mode_.width;
    desc.height = mode_.height;
    const PixelFormat format = PrimaryFormat();

    Surface *front = new Surface(*this, SurfaceRole::kPrimary, desc, format, nullptr, 0);
    Surface *previous = front;
    bool valid = front->Valid();
    for (std::uint32_t i = 0; i < backBuffers; ++i) {
        Surface *back = new Surface(*this, SurfaceRole::kBackBuffer, desc, format, front, 0);
        front->chain_.push_back(back);
        previous->next_ = back;
        previous = back;
        valid = valid && back->Valid();
    }
    if (!valid) {
        front->Release();
        return kDdErrOutOfMemory;
    }
    primary_ = front;
    *out = front;
    return kDdOk;
}

HResult DirectDraw::CreatePalette(std::uint32_t caps, const PaletteColor *entries, Palette **out) {
    if (out == nullptr) {
        return kDdErrInvalidParams;
    }
    *out = nullptr;
    if ((caps & (kDdpcaps1Bit | kDdpcaps2Bit | kDdpcaps4Bit)) != 0) {
        return kDdErrUnsupported;
    }
    if ((caps & kDdpcaps8Bit) == 0 || entries == nullptr) {
        return kDdErrInvalidParams;
    }
    *out = new Palette(*this, caps, entries); // the game's reference
    return kDdOk;
}

HResult DirectDraw::CreateClipper(std::uint32_t flags, Clipper **out) {
    if (out == nullptr || flags != 0) {
        return kDdErrInvalidParams;
    }
    *out = new Clipper(this); // the game's reference
    return kDdOk;
}

HResult DirectDraw::GetGdiSurface(Surface **out) {
    if (out == nullptr) {
        return kDdErrInvalidParams;
    }
    *out = primary_;
    if (primary_ == nullptr) {
        return kDdErrNotFound;
    }
    primary_->AddRef();
    return kDdOk;
}

HResult DirectDraw::WaitForVerticalBlank(std::uint32_t flags) {
    return (flags & kDdwaitvbBlockBeginEvent) != 0 ? kDdErrUnsupported : kDdOk;
}

HResult DirectDraw::GetVerticalBlankStatus(bool &inBlank) const {
    inBlank = false;
    return kDdOk;
}

HResult DirectDraw::GetScanLine(std::uint32_t &line) const {
    line = 0;
    return kDdOk;
}

HResult DirectDraw::GetMonitorFrequency(std::uint32_t &hertz) const {
    hertz = mode_.refreshRate;
    return kDdOk;
}

HResult DirectDraw::GetAvailableVidMem(const SurfaceCaps &, std::uint32_t *total,
                                       std::uint32_t *free) const {
    if (total == nullptr && free == nullptr) {
        return kDdErrInvalidParams;
    }
    if (total != nullptr) {
        *total = kVideoMemoryTotal;
    }
    if (free != nullptr) {
        *free = kVideoMemoryFree;
    }
    return kDdOk;
}

HResult DirectDraw::GetDeviceIdentifier(std::uint32_t, DeviceIdentifier &out) const {
    out = DeviceIdentifier{"display", "Display adapter", 0, 0};
    return kDdOk;
}

platform::Result<platform::TextureId> DirectDraw::DisplayTexture() {
    const DirectDraw *owner = displayOwner(gpu_);
    if (owner != nullptr && owner != this) {
        return Error::kWrongState; // another object's back buffer: never taken lazily
    }
    if (!displayTexture_.Valid() || owner != this) {
        const PixelFormat format =
            mode_.bitsPerPixel == 8 ? PixelFormat::kXrgb8888 : PrimaryFormat();
        platform::Result<platform::TextureId> back =
            gpu_.ConfigureBackBuffer(mode_.width, mode_.height, format);
        if (!back.Ok()) {
            return back.Code();
        }
        displayTexture_ = back.Value();
        displayOwners()[&gpu_] = this;
    }
    return displayTexture_;
}

bool DirectDraw::OwnsDisplay() const {
    return displayOwner(gpu_) == this && displayTexture_.Valid();
}

DirectDraw::~DirectDraw() {
    if (displayOwner(gpu_) == this) {
        displayOwners().erase(&gpu_);
    }
}

void DirectDraw::ForgetDisplayOwners() {
    displayOwners().clear();
}

PixelFormat DirectDraw::PrimaryFormat() const {
    return displayFormat(mode_.bitsPerPixel).value_or(PixelFormat::kRgb565);
}

HResult DirectDraw::PresentPrimary(Surface &primary) {
    platform::Result<platform::TextureId> shown = primary.GpuTexture();
    if (!shown.Ok()) {
        return fromError(shown.Code());
    }
    platform::Result<platform::TextureId> display = DisplayTexture();
    if (!display.Ok()) {
        return fromError(display.Code());
    }
    if (shown.Value() != display.Value()) {
        // An 8-bit primary, converted through its palette; or one made for another mode.
        platform::BlitDesc blit;
        blit.source = shown.Value();
        blit.destination = display.Value();
        if (primary.Width() != mode_.width || primary.Height() != mode_.height ||
            primary.Format() != PixelFormat::kIndexed8) {
            return kDdOk; // nothing of this primary can be shown in the current mode
        }
        const platform::Status copied = gpu_.Blit(blit);
        if (!copied.Ok()) {
            return fromError(copied.Code());
        }
    }
    // The frame scaled into a window of another size: linear or nearest (fix_smooth_scaled_frame).
    // A Gpu without the call keeps its own; nothing else depends on it.
    const platform::TextureFilter filter = fixes::isEnabled("fix_smooth_scaled_frame")
                                               ? platform::TextureFilter::kLinear
                                               : platform::TextureFilter::kPoint;
    if (presentFilter_ != filter) {
        (void)gpu_.SetPresentFilter(filter);
        presentFilter_ = filter;
    }
    // At the largest whole-number scale that fits, or any scale (fix_integer_scaled_frame); the
    // window keeps it for the Gpu's placement and the mouse mapping alike.
    (void)window_.SetWholeScale(fixes::isEnabled("fix_integer_scaled_frame"));
    const platform::Status presented = gpu_.Present();
    if (!presented.Ok()) {
        return fromError(presented.Code());
    }
    ++counts_.presents;
    if (g_presentHook != nullptr) {
        g_presentHook(*this);
    }
    return kDdOk;
}

void DirectDraw::SetPresentHook(PresentHook hook) {
    g_presentHook = hook;
}

void DirectDraw::SurfaceGone(Surface &surface) {
    if (primary_ == &surface) {
        primary_ = nullptr;
    }
}

void DirectDraw::PaletteChanged(Palette &palette) {
    if (primary_ != nullptr && primary_->AttachedPalette() == &palette && mode_.bitsPerPixel == 8) {
        (void)PresentPrimary(*primary_);
    }
}

} // namespace sfcop::hle::ddraw4
