#include "hle/gdi32/Screen.h"

#include "platform/Window/WindowMock.h"

#include <doctest/doctest.h>

#include <optional>

namespace sfcop::hle::gdi32 {
namespace {

TEST_CASE("A window's logical size is the screen, whatever the display mode") {
    const ScreenInfo got = NextScreen(ScreenInfo{}, Size{1024, 768}, Size{800, 600});
    CHECK(got.width == 1024);
    CHECK(got.height == 768);
    CHECK(got.bitsPerPixel == 32);
}

TEST_CASE("With no window (headless), the display mode is the screen") {
    const ScreenInfo got = NextScreen(ScreenInfo{}, std::nullopt, Size{800, 600});
    CHECK(got.width == 800);
    CHECK(got.height == 600);
    CHECK(got.bitsPerPixel == 32);
}

TEST_CASE("A window of no size, or no window and no mode, leaves the screen as it was") {
    ScreenInfo before;
    before.width = 800;
    before.height = 600;
    const ScreenInfo hidden = NextScreen(before, Size{0, 0}, Size{640, 480});
    CHECK(hidden.width == 800);
    CHECK(hidden.height == 600);
    const ScreenInfo unknown = NextScreen(ScreenInfo{}, std::nullopt, Size{0, 0});
    CHECK(unknown.width == 640);
    CHECK(unknown.height == 480);
}

TEST_CASE("Headless before SetDisplayMode, win32hle's mode is 640x480: taken as it is") {
    const ScreenInfo got = NextScreen(ScreenInfo{}, std::nullopt, Size{640, 480});
    CHECK(got.width == 640);
    CHECK(got.height == 480);
}

TEST_CASE("ReadScreen: a window whose GetState fails (headless) gives the display mode") {
    platform::WindowMock window;
    window.stateResult = platform::Error::kUnsupported;
    const ScreenInfo got = ReadScreen(ScreenInfo{}, window, Size{800, 600});
    CHECK(got.width == 800);
    CHECK(got.height == 600);
}

TEST_CASE("ReadScreen: a window that answers its size gives that size, not the mode") {
    platform::WindowMock window;
    window.stateResult =
        platform::WindowState{platform::WindowMode::kWindowed, 1024, 768, 2048, 1536, true, false};
    const ScreenInfo got = ReadScreen(ScreenInfo{}, window, Size{800, 600});
    CHECK(got.width == 1024);
    CHECK(got.height == 768);
}

} // namespace
} // namespace sfcop::hle::gdi32
