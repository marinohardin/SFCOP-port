#include "hle/ddraw4/Palette.h"
#include "hle/ddraw4/DirectDraw.h"

namespace sfcop::hle::ddraw4 {

Palette::Palette(DirectDraw &owner, std::uint32_t caps, const PaletteColor *entries)
    : owner_(owner), caps_(caps) {
    owner_.AdoptChild();
    if (entries != nullptr) {
        for (std::size_t i = 0; i < kPaletteEntries; ++i) {
            entries_[i] = entries[i];
        }
    }
}

Palette::~Palette() {
    if (gpuPalette_.Valid()) {
        (void)owner_.Device().DestroyPalette(gpuPalette_);
    }
    owner_.ChildGone();
}

HResult Palette::GetCaps(std::uint32_t &caps) const {
    caps = caps_;
    return kDdOk;
}

HResult Palette::GetEntries(std::uint32_t, std::uint32_t start, std::uint32_t count,
                            PaletteColor *out) const {
    if (out == nullptr || start >= kPaletteEntries || count > kPaletteEntries - start) {
        return kDdErrInvalidParams;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        out[i] = entries_[start + i];
    }
    return kDdOk;
}

HResult Palette::SetEntries(std::uint32_t, std::uint32_t start, std::uint32_t count,
                            const PaletteColor *entries) {
    if (entries == nullptr || count == 0 || start >= kPaletteEntries ||
        count > kPaletteEntries - start) {
        return kDdErrInvalidParams;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        entries_[start + i] = entries[i];
    }
    if (gpuPalette_.Valid()) {
        const std::array<platform::PaletteEntry, kPaletteEntries> colors = gpuEntries();
        const platform::Status written =
            owner_.Device().WritePalette(gpuPalette_, start, count, colors.data() + start);
        if (!written.Ok()) {
            return fromError(written.Code());
        }
    }
    owner_.PaletteChanged(*this);
    return kDdOk;
}

platform::Result<platform::PaletteId> Palette::GpuPalette() {
    if (!gpuPalette_.Valid()) {
        const std::array<platform::PaletteEntry, kPaletteEntries> colors = gpuEntries();
        platform::Result<platform::PaletteId> made = owner_.Device().CreatePalette(colors.data());
        if (!made.Ok()) {
            return made.Code();
        }
        gpuPalette_ = made.Value();
    }
    return gpuPalette_;
}

std::array<platform::PaletteEntry, kPaletteEntries> Palette::gpuEntries() const {
    std::array<platform::PaletteEntry, kPaletteEntries> colors{};
    const bool alpha = (caps_ & kDdpcapsAlpha) != 0;
    for (std::size_t i = 0; i < kPaletteEntries; ++i) {
        colors[i] = platform::PaletteEntry{entries_[i].red, entries_[i].green, entries_[i].blue,
                                           alpha ? entries_[i].flags : std::uint8_t{255}};
    }
    return colors;
}

} // namespace sfcop::hle::ddraw4
