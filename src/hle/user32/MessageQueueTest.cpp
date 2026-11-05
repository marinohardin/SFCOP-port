#include "hle/user32/MessageQueue.h"
#include "hle/user32/User32TestRig.h"

#include <doctest/doctest.h>

namespace sfcop::hle::user32 {

using platform::MouseButton;
using platform::WindowEventType;
using test::Rig;

TEST_CASE("Posted messages come back in order, then WM_QUIT, then WM_PAINT") {
    Rig rig;
    const std::uint32_t hwnd = rig.GameWindow();
    rig.user.Post(hwnd, 0x0400, 1, 2);
    rig.user.Post(hwnd, 0x0401, 3, 4);
    rig.user.Queue().PostQuit(7);
    Retrieved got = rig.user.Peek(0, 0, 0, true);
    REQUIRE(got.found);
    CHECK(got.message.message == 0x0400);
    CHECK(got.message.wParam == 1);
    CHECK(got.message.time == 1000);
    got = rig.user.Peek(0, 0, 0, true);
    CHECK(got.message.message == 0x0401);
    got = rig.user.Peek(0, 0, 0, false);
    CHECK(got.message.message == kWmQuit);
    CHECK(got.message.wParam == 7);
    got = rig.user.Peek(0, 0, 0, true);
    CHECK(got.message.message == kWmQuit);
    // The window was shown: it stays invalid until painted.
    got = rig.user.Peek(0, 0, 0, true);
    REQUIRE(got.found);
    CHECK(got.message.message == kWmPaint);
    CHECK(got.message.hwnd == hwnd);
    got = rig.user.Peek(0, 0, 0, true);
    CHECK(got.message.message == kWmPaint); // still: nothing validated it
    rig.user.Dispatch(got.message);         // DefWindowProcA validates
    CHECK_FALSE(rig.user.Peek(0, 0, 0, true).found);
}

TEST_CASE("PeekMessageA's window and range filters; PM_NOREMOVE leaves the message") {
    Rig rig;
    const std::uint32_t a = rig.GameWindow();
    const std::uint32_t b = rig.GameWindow();
    rig.Drain();
    rig.user.Post(a, 0x0400, 0, 0);
    rig.user.Post(b, 0x0500, 0, 0);
    rig.user.Post(0, 0x0600, 0, 0);
    CHECK(rig.user.Peek(b, 0, 0, false).message.message == 0x0500);
    CHECK(rig.user.Peek(0, 0x0500, 0x05FF, false).message.message == 0x0500);
    CHECK(rig.user.Peek(0xFFFFFFFFu, 0, 0, true).message.message == 0x0600);
    CHECK_FALSE(rig.user.Peek(a, 0x0500, 0x0600, true).found);
    CHECK(rig.user.Queue().Size() == 2);
}

TEST_CASE("A key goes to the active window as WM_KEYDOWN with its scan code") {
    Rig rig;
    const std::uint32_t hwnd = rig.GameWindow();
    rig.Drain();
    rig.Key(0x04, true);       // A
    rig.Key(0x04, true, true); // auto-repeat
    rig.Key(0x04, false);
    const std::vector<Message> got = rig.Drain();
    REQUIRE(got.size() == 3);
    CHECK(got[0].hwnd == hwnd);
    CHECK(got[0].message == kWmKeyDown);
    CHECK(got[0].wParam == 'A');
    CHECK(got[0].lParam == 0x001E0001u);
    CHECK(got[1].lParam == 0x401E0001u);
    CHECK(got[2].message == kWmKeyUp);
    CHECK(got[2].lParam == 0xC01E0001u);
}

TEST_CASE("TranslateMessage queues the character ahead of what is waiting") {
    Rig rig;
    rig.GameWindow();
    rig.Drain();
    rig.Key(0xE1, true); // left Shift
    rig.Key(0x1E, true); // 1
    rig.user.Pump();
    Retrieved got = rig.user.Peek(0, 0, 0, true);
    CHECK(got.message.wParam == kVkShift);
    CHECK_FALSE(rig.user.Translate(got.message));
    got = rig.user.Peek(0, 0, 0, true);
    CHECK(got.message.wParam == '1');
    CHECK(rig.user.Translate(got.message));
    got = rig.user.Peek(0, 0, 0, true);
    CHECK(got.message.message == kWmChar);
    CHECK(got.message.wParam == '!');
    CHECK(got.message.lParam == 0x00020001u);
}

TEST_CASE("Alt makes keys system keys; F10 alone is one") {
    Rig rig;
    rig.GameWindow();
    rig.Drain();
    rig.Key(0xE2, true);  // left Alt
    rig.Key(0x3D, true);  // F4
    rig.Key(0xE2, false);
    rig.Key(0x43, true);  // F10
    const std::vector<Message> got = rig.Drain();
    REQUIRE(got.size() == 4);
    CHECK(got[0].message == kWmSysKeyDown);
    CHECK(got[0].wParam == kVkMenu);
    CHECK(got[1].message == kWmSysKeyDown);
    CHECK(got[1].wParam == kVkF4);
    CHECK((got[1].lParam & (1u << 29)) != 0);
    CHECK(got[2].message == kWmSysKeyUp);
    CHECK(got[3].message == kWmSysKeyDown);
    CHECK(got[3].wParam == 0x79);
}

TEST_CASE("GetKeyState follows the queue: bit 15 down, bit 0 toggled") {
    Rig rig;
    rig.GameWindow();
    rig.Key(0x39, true); // Caps Lock
    rig.Key(0x39, false);
    rig.Key(0xE0, true); // left Control
    rig.Drain();
    CHECK(rig.user.KeyState(kVkCapital) == 0x0001);
    CHECK(rig.user.KeyState(kVkControl) == 0x8001);
    CHECK(rig.user.KeyState(kVkLControl) == 0x8001);
    CHECK(rig.user.KeyState('Z') == 0);
}

TEST_CASE("GetAsyncKeyState reads the Input component now") {
    Rig rig;
    rig.input.keyboard[0xE5] = true; // right Shift
    rig.input.mouse.buttons[static_cast<std::size_t>(MouseButton::kRight)] = true;
    CHECK(rig.user.AsyncKeyState(kVkShift) == 0x8000);
    CHECK(rig.user.AsyncKeyState(kVkRShift) == 0x8000);
    CHECK(rig.user.AsyncKeyState(kVkLShift) == 0);
    CHECK(rig.user.AsyncKeyState(kVkRButton) == 0x8000);
    CHECK(rig.user.AsyncKeyState(kVkLButton) == 0);
    rig.input.failWith = platform::Error::kUnsupported;
    CHECK(rig.user.AsyncKeyState(kVkShift) == 0);
}

TEST_CASE("The mouse goes to the active window in its client coordinates, scaled") {
    Rig rig;
    const std::uint32_t hwnd = rig.GameWindow();
    rig.Drain();
    // The host window is twice the screen: the platform reports logical pixels of 1280x960.
    rig.window.stateResult = platform::WindowState{platform::WindowMode::kWindowed, 1280, 960,
                                                   1280, 960, true, false};
    rig.Mouse(WindowEventType::kMouseMove, 200, 100);
    rig.input.keyboard[0xE1] = true;
    rig.Key(0xE1, true);
    rig.Mouse(WindowEventType::kMouseButtonDown, 202, 102);
    rig.Mouse(WindowEventType::kMouseButtonUp, 202, 102);
    rig.Mouse(WindowEventType::kMouseButtonDown, 0, 0, MouseButton::kX1);
    const std::vector<Message> got = rig.Drain();
    REQUIRE(got.size() == 4);
    CHECK(got[0].hwnd == hwnd);
    CHECK(got[0].message == kWmMouseMove);
    CHECK(got[0].lParam == ((50u << 16) | 100u));
    CHECK(got[0].wParam == 0);
    CHECK(got[1].message == kWmKeyDown);
    CHECK(got[2].message == kWmLButtonDown);
    CHECK(got[2].wParam == (kMkLButton | kMkShift));
    CHECK(got[2].lParam == ((51u << 16) | 101u));
    CHECK(got[3].message == kWmLButtonUp);
    CHECK(got[3].wParam == kMkShift);
    std::int32_t x = 0, y = 0;
    rig.user.CursorPos(x, y);
    CHECK(x == 101);
    CHECK(y == 51);
}

TEST_CASE("Mouse coordinates are relative to the client area and clamped to the screen") {
    Rig rig;
    CreateParams params;
    params.className = "GameWindow";
    params.style = kWsPopup | kWsCaption | kWsVisible;
    params.x = 100, params.y = 100, params.width = 200, params.height = 100;
    const std::uint32_t hwnd = rig.user.CreateWindow(params, 0);
    rig.Drain();
    rig.Mouse(WindowEventType::kMouseButtonDown, 90, 90, MouseButton::kRight);
    rig.Mouse(WindowEventType::kMouseMove, 700, -5);
    const std::vector<Message> got = rig.Drain();
    REQUIRE(got.size() == 2);
    CHECK(got[0].hwnd == hwnd);
    CHECK(got[0].message == kWmRButtonDown);
    // (90, 90) on the screen is left of and above the client origin (103, 121): (-13, -31).
    CHECK(got[0].lParam == 0xFFE1FFF3u);
    std::int32_t x = 0, y = 0;
    rig.user.CursorPos(x, y);
    CHECK(x == 639);
    CHECK(y == 0);
}

TEST_CASE("The wheel goes to the focus window in notches of 120") {
    Rig rig;
    const std::uint32_t hwnd = rig.GameWindow();
    rig.Drain();
    platform::WindowEvent wheel;
    wheel.type = WindowEventType::kMouseWheel;
    wheel.input.x = 10;
    wheel.input.y = 20;
    wheel.input.wheel = -1.0f;
    rig.Event(wheel);
    const std::vector<Message> got = rig.Drain();
    REQUIRE(got.size() == 1);
    CHECK(got[0].hwnd == hwnd);
    CHECK(got[0].message == kWmMouseWheel);
    CHECK(got[0].wParam == 0xFF880000u); // -120 in the high word
    CHECK(got[0].lParam == ((20u << 16) | 10u));
}

TEST_CASE("Focus lost and regained deactivate and reactivate the game's window") {
    Rig rig;
    const std::uint32_t hwnd = rig.GameWindow();
    rig.Drain();
    platform::WindowEvent lost;
    lost.type = WindowEventType::kFocusLost;
    rig.Event(lost);
    std::vector<Message> got = rig.Drain();
    REQUIRE(got.size() == 3);
    CHECK(got[0].message == kWmActivate);
    CHECK(got[0].wParam == kWaInactive);
    CHECK(got[1].message == kWmActivateApp);
    CHECK(got[1].wParam == 0);
    CHECK(got[2].message == kWmKillFocus);
    CHECK(rig.user.Windows().Active() == 0);
    rig.Key(0x04, true);
    CHECK(rig.Drain().empty()); // no window has the keyboard

    platform::WindowEvent gained;
    gained.type = WindowEventType::kFocusGained;
    rig.Event(gained);
    got = rig.Drain();
    REQUIRE(got.size() == 3);
    CHECK(got[0].message == kWmActivateApp);
    CHECK(got[0].wParam == 1);
    CHECK(got[1].message == kWmActivate);
    CHECK(got[1].wParam == kWaActive);
    CHECK(got[2].message == kWmSetFocus);
    CHECK(rig.user.Windows().Active() == hwnd);
}

TEST_CASE("The close box posts WM_CLOSE to the active window, not the first one made") {
    Rig rig;
    CreateParams debug;
    debug.className = "GameWindow";
    debug.style = kWsPopup;
    const std::uint32_t first = rig.user.CreateWindow(debug, 0);
    rig.user.ShowWindow(first, kSwShowNoActivate);
    const std::uint32_t game = rig.GameWindow();
    rig.Drain();
    platform::WindowEvent close;
    close.type = WindowEventType::kCloseRequested;
    rig.Event(close);
    const std::vector<Message> got = rig.Drain();
    REQUIRE(got.size() == 1);
    CHECK(got[0].hwnd == game);
    CHECK(got[0].message == kWmClose);
}

TEST_CASE("GetMessageA waits, pumping, until a message comes") {
    Rig rig;
    const std::uint32_t hwnd = rig.GameWindow();
    rig.Drain();
    rig.guest.onIdle = [&] {
        if (rig.guest.idled == 3) {
            rig.Key(0x29, true); // Escape
        }
    };
    const Message got = rig.user.Get(0, 0, 0);
    CHECK(rig.guest.idled == 3);
    CHECK(got.hwnd == hwnd);
    CHECK(got.message == kWmKeyDown);
    CHECK(got.wParam == kVkEscape);
}

TEST_CASE("The pump is the host's, run on the main thread; its events reach the queue once") {
    Rig rig;
    int hostPumps = 0;
    rig.user.SetHostPump([&] { ++hostPumps; });
    rig.user.Pump();
    CHECK(hostPumps == 1);
    CHECK(rig.window.log.Count("PumpEvents") == 0); // the module does not drain the Window

    const std::uint32_t hwnd = rig.GameWindow();
    rig.Drain();
    rig.user.OnWindowEvent([] {
        platform::WindowEvent close;
        close.type = platform::WindowEventType::kCloseRequested;
        return close;
    }());
    const std::vector<Message> messages = rig.Drain();
    REQUIRE(messages.size() == 1);
    CHECK(messages[0].hwnd == hwnd);
    CHECK(messages[0].message == kWmClose);
}

TEST_CASE("A destroyed window's messages are dropped") {
    Rig rig;
    const std::uint32_t hwnd = rig.GameWindow();
    rig.Drain();
    rig.user.Post(hwnd, 0x0400, 0, 0);
    rig.user.DestroyWindow(hwnd);
    CHECK_FALSE(rig.user.Peek(0, 0, 0, true).found);
    CHECK_FALSE(rig.user.Post(hwnd, 0x0400, 0, 0));
}

TEST_CASE("A close request before any window exists goes to the first window made") {
    Rig rig;
    rig.Event(platform::WindowEvent{}); // kCloseRequested is the default type
    rig.user.Pump();
    CHECK(rig.user.Queue().Size() == 0);
    // The rig has made no window yet: the request waits.
    CreateParams params;
    params.className = "GameWindow";
    params.style = kWsPopup;
    params.width = 640;
    params.height = 480;
    const std::uint32_t first = rig.user.CreateWindow(params, 0);
    REQUIRE(first != 0);
    const std::vector<Message> messages = rig.Drain();
    REQUIRE(messages.size() == 1);
    CHECK(messages[0].hwnd == first);
    CHECK(messages[0].message == kWmClose);
    // Once; a window made later gets nothing.
    CHECK(rig.user.CreateWindow(params, 0) != 0);
    CHECK(rig.Drain().empty());
}

TEST_CASE("GetMessageA on another thread takes nothing and never pumps, until the host stops") {
    platform::WindowMock window;
    platform::InputMock input;
    gdi32::HeapPixelMemory memory;
    gdi32::Gdi gdi{memory, gdi32::countingHandles(), [] { return gdi32::ScreenInfo{}; }};
    test::FakeGuest guest;
    WindowHost handles = countingUserHandles([] { return gdi32::ScreenInfo{}; });
    handles.onMainThread = [] { return false; }; // this thread is not the main one
    User32 user{window, input, gdi, guest, std::move(handles), [] { return 1000u; }};
    int pumps = 0;
    user.SetHostPump([&] { ++pumps; });
    bool stopping = false;
    user.SetStopping([&] { return stopping; });
    user.Queue().Post(0, 0x0400, 1, 2); // the main thread's message
    guest.onIdle = [&] { stopping = guest.idled >= 3; };

    const Message got = user.Get(0, 0, 0);
    CHECK(got.message == kWmQuit);
    CHECK(guest.idled == 3);
    CHECK(pumps == 0);
    CHECK(window.log.Count("PumpEvents") == 0);
    CHECK(user.Queue().Size() == 1); // the posted message is still there
}

TEST_CASE("The module never drains the Window itself: only the host's pump does") {
    Rig rig;
    rig.user.SetHostPump({});
    rig.Event(platform::WindowEvent{});
    rig.user.Pump();
    CHECK(rig.window.log.Count("PumpEvents") == 0);
    CHECK(rig.window.log.Count("PollEvent") == 0);
}

} // namespace sfcop::hle::user32
