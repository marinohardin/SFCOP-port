// Screen: the size of the game's display, which GetDeviceCaps answers and USER32 keeps the cursor
// inside. With a window it is the window's logical size, which follows DirectDraw's display mode.
// A headless run has no window (the stub's GetState fails); the display mode DirectDraw recorded
// is then the size, so that a click anywhere on the game's 800x600 menus lands where it was
// aimed and is not clamped to the 640x480 the screen starts at.
#pragma once

#include "hle/gdi32/Gdi.h"
#include "platform/Window/Window.h"

#include <cstdint>
#include <optional>

namespace sfcop::hle::gdi32 {

// The screen after `previous`, given the window's logical size (none when there is no window)
// and DirectDraw's display mode (0 by 0 when unknown). A window's size wins; a window of no size
// (not shown yet, minimised) leaves the screen as it was; with no window the mode is the size.
// The depth stays 32 bits either way.
inline ScreenInfo NextScreen(ScreenInfo previous, std::optional<Size> window, Size displayMode) {
    const Size size = window ? *window : displayMode;
    if (size.cx > 0 && size.cy > 0) {
        previous.width = size.cx;
        previous.height = size.cy;
    }
    return previous;
}

// NextScreen on what the game's window answers now: no window when its GetState fails (the
// headless stub). `displayMode` is win32hle's DirectDraw's, which is 640x480 until the game sets
// a mode, so a headless screen is 640x480 before SetDisplayMode, as it always was.
inline ScreenInfo ReadScreen(ScreenInfo previous, platform::Window &window, Size displayMode) {
    std::optional<Size> size;
    if (const auto state = window.GetState(); state.Ok()) {
        size = Size{static_cast<std::int32_t>(state.Value().width),
                    static_cast<std::int32_t>(state.Value().height)};
    }
    return NextScreen(previous, size, displayMode);
}

} // namespace sfcop::hle::gdi32
