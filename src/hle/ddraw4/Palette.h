// IDirectDrawPalette: 256 colours for 8-bit surfaces, kept here and mirrored into a Gpu palette
// once a surface needs one.
#pragma once

#include "hle/ddraw4/ComObject.h"
#include "hle/ddraw4/Ddraw4Defs.h"

#include <array>

namespace sfcop::hle::ddraw4 {

class DirectDraw;

// DDPCAPS bits the palette keeps.
constexpr std::uint32_t kDdpcaps4Bit = 0x00000001;
constexpr std::uint32_t kDdpcaps8BitEntries = 0x00000002;
constexpr std::uint32_t kDdpcaps1Bit = 0x00000100;
constexpr std::uint32_t kDdpcaps2Bit = 0x00000200;
constexpr std::uint32_t kDdpcapsAlpha = 0x00000400;

constexpr std::size_t kPaletteEntries = 256;

class Palette final : public ComObject {
public:
    // Use DirectDraw::CreatePalette(), which checks `caps`.
    Palette(DirectDraw &owner, std::uint32_t caps, const PaletteColor *entries);

    // DDPCAPS of the palette as created.
    HResult GetCaps(std::uint32_t &caps) const;

    // Copies `count` entries from `start` into `out`. DDERR_INVALIDPARAMS for a range past 256.
    HResult GetEntries(std::uint32_t flags, std::uint32_t start, std::uint32_t count,
                       PaletteColor *out) const;

    // Replaces `count` entries from `start`; an 8-bit primary showing this palette is shown
    // again. DDERR_INVALIDPARAMS for a range past 256 or no entries.
    HResult SetEntries(std::uint32_t flags, std::uint32_t start, std::uint32_t count,
                       const PaletteColor *entries);

    // The palette is initialised when created: DDERR_ALREADYINITIALIZED.
    HResult Initialize() const { return kDdErrAlreadyInitialized; }

    // The Gpu palette with these colours, created on first use.
    platform::Result<platform::PaletteId> GpuPalette();

    const std::array<PaletteColor, kPaletteEntries> &Entries() const { return entries_; }
    DirectDraw &Owner() const { return owner_; }

private:
    ~Palette() override;

    std::array<platform::PaletteEntry, kPaletteEntries> gpuEntries() const;

    DirectDraw &owner_;
    std::uint32_t caps_;
    std::array<PaletteColor, kPaletteEntries> entries_{};
    platform::PaletteId gpuPalette_;
};

} // namespace sfcop::hle::ddraw4
