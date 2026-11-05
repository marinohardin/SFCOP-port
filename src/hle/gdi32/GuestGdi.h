// The guest side the GDI32 and USER32 shims share (Gdi32Guest.cpp, User32Guest.cpp): the one Gdi
// the game's GDI rows and USER32's drawing work on, with its pixels in guest memory and its
// handles in win32hle's handle table, and the screen the game sees. Included only by the shim
// files, which the lift alone compiles.
//
// Handles: DCs are type 30 and GDI objects type 31 of win32hle's table (src/hle/README.md,
// "Handle types"). A DC this module did not make is win32hle's (DirectDraw's surface DCs,
// through hle_gdi_surface_dc): the rows hand a call on such a DC to win32hle's own row.
#pragma once

#include "hle/gdi32/Bitmap.h"
#include "hle/gdi32/Gdi.h"
#include "hle/user32/User32.h"

extern "C" {
#include "win32hle.h"
}

#include <cstdint>
#include <cstring>
#include <memory>

namespace sfcop::hle::gdi32::guest {

constexpr int kDcHandleType = 30;
constexpr int kObjectHandleType = 31;

// What win32hle's table holds for this module's handles: nothing is looked up through it.
inline char g_handleMarker;

// Pixels the game may be handed (a DIB section's bits): in guest memory.
class GuestPixelMemory final : public PixelMemory {
public:
    std::uint8_t *Allocate(std::size_t bytes) override {
        if (bytes == 0 || bytes > 0x7FFFFFFFu) {
            return nullptr;
        }
        return static_cast<std::uint8_t *>(hle_alloc(static_cast<std::uint32_t>(bytes)));
    }
    void Free(std::uint8_t *block) override {
        if (block != nullptr) {
            hle_free(block);
        }
    }
};

// The game's screen: the platform window's logical size, which follows DirectDraw's display
// mode, read on the main thread; 640x480 until a window says otherwise. Always 32 bits.
inline ScreenInfo screen() {
    static ScreenInfo cached;
    if (user32::OnMainThread()) { // true only once the host is attached
        const auto state = user32::AttachedHost()->window->GetState();
        if (state.Ok() && state.Value().width != 0 && state.Value().height != 0) {
            cached.width = static_cast<std::int32_t>(state.Value().width);
            cached.height = static_cast<std::int32_t>(state.Value().height);
        }
    }
    return cached;
}

inline Gdi &gdi() {
    static GuestPixelMemory memory;
    static Gdi shared(
        memory,
        HandleSource{[](HandleKind kind) {
                         return hle_handle_alloc(
                             kind == HandleKind::kDc ? kDcHandleType : kObjectHandleType,
                             &g_handleMarker);
                     },
                     [](std::uint32_t handle) { hle_handle_close(handle); }},
        [] { return screen(); });
    return shared;
}

// win32hle's own row of this name (GDI32's or USER32's), which this module's registration hides.
inline recomp_func_t win32hleRow(const char *name) {
    for (const win32hle_shim *table : {win32hle_gdidc, win32hle_gdi32, win32hle_user32}) {
        for (const win32hle_shim *entry = table; entry->name != nullptr; ++entry) {
            if (std::strcmp(entry->name, name) == 0) {
                return entry->fn;
            }
        }
    }
    return nullptr;
}

// Whether a DC is someone else's (0 is nobody's): the row then hands the call on.
inline bool foreignDc(std::uint32_t dc) {
    return dc != 0 && !gdi().OwnsDc(dc);
}

} // namespace sfcop::hle::gdi32::guest
