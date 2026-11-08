// The DirectDraw object behind IDirectDraw, IDirectDraw2 and IDirectDraw4 (DESIGN §5.6, §6.1):
// one object, three views (ComObject.h). It owns the display: the mode the game set, the Gpu back
// buffer that shows it, and the primary surface the game draws into.
//
// The display is the Gpu's back buffer at the game's mode. At 16 and 32 bits per pixel the primary
// surface draws straight into it; at 8 bits the primary is an indexed texture converted through
// its palette on the way to a 32-bit back buffer. The back buffer is made on first use, so a mode
// is shown only once something is drawn. Before SetDisplayMode the mode is 640x480 at 16 bits.
#pragma once

#include "hle/ddraw4/ComObject.h"
#include "hle/ddraw4/Ddraw4Defs.h"
#include "hle/ddraw4/PixelMemory.h"

#include "platform/Gpu/Gpu.h"
#include "platform/Window/Window.h"

#include <optional>
#include <string>
#include <vector>

namespace sfcop::hle::ddraw4 {

class Clipper;
class Palette;
class Surface;

// The DDDEVICEIDENTIFIER fields the module fills; the rest are zero.
struct DeviceIdentifier {
    std::string driver;
    std::string description;
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
};

// The display mode the game set.
struct DisplayModeState {
    std::uint32_t width = 640;
    std::uint32_t height = 480;
    std::uint32_t bitsPerPixel = 16;
    std::uint32_t refreshRate = 60;
};

class DirectDraw final : public ComObject {
public:
    // The Gpu must be attached to `window`. Both, and `memory`, outlive the object.
    DirectDraw(platform::Gpu &gpu, platform::Window &window, PixelMemory &memory);

    // IDirectDraw::SetCooperativeLevel. Fullscreen needs exclusive and exclusive needs
    // fullscreen; normal goes with neither (DDERR_INVALIDPARAMS otherwise). The window handle is
    // kept for clippers; the Window component is the one window whatever the handle.
    HResult SetCooperativeLevel(std::uint32_t window, std::uint32_t flags);

    // Sets the game's resolution and colour depth (8, 16 or 32 bits; anything else is
    // DDERR_INVALIDMODE). The window takes the size in its current mode, and the back buffer is
    // made again at the new size. The refresh rate is recorded, never applied.
    HResult SetDisplayMode(std::uint32_t width, std::uint32_t height, std::uint32_t bitsPerPixel,
                           std::uint32_t refreshRate, std::uint32_t flags);

    // Nothing to restore: the window keeps its size until the game sets another mode.
    HResult RestoreDisplayMode() { return kDdOk; }

    // The current mode as a DDSURFACEDESC: size, pitch, pixel format and refresh rate.
    HResult GetDisplayMode(SurfaceDesc &out) const;

    // The modes the game may choose: the classic 4:3 resolutions and those the display lists, no
    // larger than the desktop, smallest first, at 8, 16 and then 32 bits, matching `filter`'s
    // width, height and bit count where it sets them.
    HResult EnumDisplayModes(std::uint32_t flags, const SurfaceDesc *filter,
                             std::vector<SurfaceDesc> &out);

    // The capabilities this module serves, the same for the driver and the emulation layer.
    HResult GetCaps(DdCaps *driver, DdCaps *emulation) const;

    // Creates a surface from a DDSURFACEDESC(2): a primary (with a flipping chain of up to 3
    // back buffers when it asks for one), a plain offscreen surface, a texture (with its mipmap
    // levels) or a 16-bit z-buffer. DDERR_INVALIDCAPS, DDERR_INVALIDPARAMS and
    // DDERR_INVALIDPIXELFORMAT for what the Gpu cannot hold.
    HResult CreateSurface(const SurfaceDesc &desc, Surface **out);

    // Creates an 8-bit palette. Fewer bits per entry are DDERR_UNSUPPORTED.
    HResult CreatePalette(std::uint32_t caps, const PaletteColor *entries, Palette **out);

    HResult CreateClipper(std::uint32_t flags, Clipper **out);

    // The primary surface, AddRef'd; DDERR_NOTFOUND without one.
    HResult GetGdiSurface(Surface **out);

    HResult FlipToGdiSurface() { return kDdOk; }

    // Fixed answers: the module has no vertical blank to wait for and no video memory to run out
    // of (256 MB total, 192 MB free), and the display runs at the mode's refresh rate.
    HResult WaitForVerticalBlank(std::uint32_t flags);
    HResult GetVerticalBlankStatus(bool &inBlank) const;
    HResult GetScanLine(std::uint32_t &line) const;
    HResult GetMonitorFrequency(std::uint32_t &hertz) const;
    HResult GetAvailableVidMem(const SurfaceCaps &caps, std::uint32_t *total,
                               std::uint32_t *free) const;
    HResult GetDeviceIdentifier(std::uint32_t flags, DeviceIdentifier &out) const;

    HResult Compact() { return kDdOk; }
    HResult TestCooperativeLevel() const { return kDdOk; }
    HResult RestoreAllSurfaces() { return kDdOk; }
    HResult Initialize() const { return kDdErrAlreadyInitialized; }

    // What the surfaces and the Direct3D module use.
    platform::Gpu &Device() const { return gpu_; }
    platform::Window &HostWindow() const { return window_; }
    PixelMemory &Memory() const { return memory_; }
    const DisplayModeState &Mode() const { return mode_; }
    std::uint32_t CooperativeWindow() const { return cooperativeWindow_; }

    // The back buffer at the current mode, made on first call after a mode change. The Gpu has
    // one back buffer: only SetDisplayMode takes it from another DirectDraw object, so while
    // another object owns the display (OwnsDisplay()) this is kWrongState and nothing is made.
    platform::Result<platform::TextureId> DisplayTexture();

    // Whether the Gpu's back buffer is this object's: it made the back buffer last, at
    // SetDisplayMode or its first draw, so the display shows what this object presents and its
    // DisplayTexture() holds the frame last presented. The game may make more than one DirectDraw
    // object; the host reads the display only from the one that owns it.
    bool OwnsDisplay() const;

    // The format the primary surface has in the current mode.
    platform::PixelFormat PrimaryFormat() const;

    // The primary the game created last and still holds, or nullptr.
    Surface *Primary() const { return primary_; }

    // Shows `primary` on the window: at 8 bits through its palette into the back buffer.
    HResult PresentPrimary(Surface &primary);

    // Called by the objects it created.
    void SurfaceGone(Surface &surface);
    void PaletteChanged(Palette &palette);

    // What reached the Gpu since the object was made, for the proof run's rate line
    // (README.md, "Proof run"): uploads of surface pixels (WriteTexture) and presents.
    struct Counts {
        std::uint64_t uploads = 0;
        std::uint64_t presents = 0;
    };
    const Counts &Counted() const { return counts_; }

    // Forgets which object owns each Gpu's display (tests only: a rig that leaked an object).
    static void ForgetDisplayOwners();

    // Called after every present, with this object (nullptr: none). The guest shims set it.
    using PresentHook = void (*)(const DirectDraw &draw);
    static void SetPresentHook(PresentHook hook);

private:
    friend class Surface; // counts its uploads (CountUpload)

    void CountUpload() { ++counts_.uploads; }

    ~DirectDraw() override;

    HResult createPrimary(const SurfaceDesc &desc, Surface **out);

    platform::Gpu &gpu_;
    platform::Window &window_;
    PixelMemory &memory_;
    DisplayModeState mode_;
    std::uint32_t cooperativeWindow_ = 0;
    std::uint32_t cooperativeFlags_ = 0;
    platform::TextureId displayTexture_; // zero until made for the current mode
    // The present filter last given to the Gpu (fix_smooth_scaled_frame), none before the first
    // present.
    std::optional<platform::TextureFilter> presentFilter_;
    Surface *primary_ = nullptr;
    Counts counts_;
};

} // namespace sfcop::hle::ddraw4
