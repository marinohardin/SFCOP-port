// What the user32 doctests share: a User32 on the Window and Input mocks, a Gdi on the host heap,
// counting handles, a 640x480 screen, a clock the test sets, and a fake of the game whose window
// procedures are lambdas the test writes and whose calls are logged.
#pragma once

#include "hle/gdi32/Bitmap.h"
#include "hle/gdi32/Gdi.h"
#include "hle/user32/User32.h"
#include "hle/user32/User32Defs.h"
#include "platform/Input/InputMock.h"
#include "platform/Window/WindowMock.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace sfcop::hle::user32::test {

// The game's procedures in a test live at these addresses.
constexpr std::uint32_t kGameProc = 0x00699BA0;
constexpr std::uint32_t kOtherProc = 0x00629C00;
constexpr std::uint32_t kGameDialogProc = 0x006AAD20;

struct Call {
    std::uint32_t proc, hwnd, message, wParam, lParam;
};

class FakeGuest final : public GuestCalls {
public:
    using Proc = std::function<std::uint32_t(const Call &)>;
    std::map<std::uint32_t, Proc> procs; // a proc without a lambda answers 0
    std::vector<Call> calls;
    std::map<std::uint32_t, std::string> written; // WriteText's output, by address
    std::uint32_t idled = 0;
    std::function<void()> onIdle;

    std::uint32_t CallProc(std::uint32_t proc, std::uint32_t hwnd, std::uint32_t message,
                           std::uint32_t wParam, std::uint32_t lParam) override {
        const Call call{proc, hwnd, message, wParam, lParam};
        calls.push_back(call);
        const auto found = procs.find(proc);
        return found == procs.end() ? 0 : found->second(call);
    }
    std::uint32_t WriteText(std::uint32_t address, std::uint32_t size,
                            const std::string &text) override {
        const std::string kept = text.substr(0, size == 0 ? 0 : size - 1);
        written[address] = kept;
        return static_cast<std::uint32_t>(kept.size());
    }
    void Idle(std::uint32_t) override {
        ++idled;
        if (onIdle) {
            onIdle();
        }
    }

    // The messages delivered to the game's procedures, in order, for one window (0: all).
    std::vector<std::uint32_t> Messages(std::uint32_t hwnd = 0) const {
        std::vector<std::uint32_t> out;
        for (const Call &call : calls) {
            if (hwnd == 0 || call.hwnd == hwnd) {
                out.push_back(call.message);
            }
        }
        return out;
    }
};

struct Rig {
    platform::WindowMock window;
    platform::InputMock input;
    gdi32::HeapPixelMemory memory;
    gdi32::Gdi gdi{memory, gdi32::countingHandles(), [] { return gdi32::ScreenInfo{}; }};
    FakeGuest guest;
    std::uint32_t now = 1000;
    User32 user{window, input, gdi, guest,
                countingUserHandles([] { return gdi32::ScreenInfo{}; }), [this] { return now; }};

    Rig() {
        // The game's procedures hand what they do not handle to DefWindowProcA, as the game's do.
        for (const std::uint32_t proc : {kGameProc, kOtherProc}) {
            guest.procs[proc] = [this](const Call &call) {
                return user.DefWindowProc(call.hwnd, call.message, call.wParam, call.lParam);
            };
        }
        WindowClass game;
        game.name = "GameWindow";
        game.proc = kGameProc;
        REQUIRE(user.RegisterClass(game) != 0);
        window.stateResult = platform::WindowState{platform::WindowMode::kWindowed, 640, 480,
                                                   640, 480, true, false};
        // The host's pump, as the guest runtime has it: the one drain of the Window, each event
        // handed to the module.
        user.SetHostPump([this] {
            static_cast<void>(window.PumpEvents());
            for (;;) {
                const auto event = window.PollEvent();
                if (!event.Ok() || !event.Value()) {
                    break;
                }
                user.OnWindowEvent(*event.Value());
            }
        });
    }

    // A top-level popup window of the game's class covering the screen.
    std::uint32_t GameWindow(std::uint32_t style = kWsPopup | kWsVisible) {
        CreateParams params;
        params.className = "GameWindow";
        params.windowName = "Game";
        params.style = style;
        params.width = 640;
        params.height = 480;
        const std::uint32_t hwnd = user.CreateWindow(params, 0x00120000);
        REQUIRE(hwnd != 0);
        return hwnd;
    }

    // A platform event, delivered by the next pump.
    void Event(platform::WindowEvent event) { window.pendingEvents.push_back(event); }
    void Key(platform::Scancode usage, bool down, bool repeat = false) {
        platform::WindowEvent event;
        event.type = down ? platform::WindowEventType::kKeyDown : platform::WindowEventType::kKeyUp;
        event.input.type = down ? platform::InputEventType::kKeyDown : platform::InputEventType::kKeyUp;
        event.input.key = usage;
        event.input.repeat = repeat;
        Event(event);
    }
    void Mouse(platform::WindowEventType type, std::int32_t x, std::int32_t y,
               platform::MouseButton button = platform::MouseButton::kLeft) {
        platform::WindowEvent event;
        event.type = type;
        event.input.x = x;
        event.input.y = y;
        event.input.button = button;
        Event(event);
    }

    // Every message the queue holds now, removed, after one pump.
    std::vector<Message> Drain() {
        std::vector<Message> out;
        for (Retrieved got = user.Peek(0, 0, 0, true); got.found;
             got = user.Peek(0, 0, 0, true)) {
            if (got.message.message == kWmPaint) {
                user.Windows().Validate(got.message.hwnd);
                continue;
            }
            out.push_back(got.message);
        }
        return out;
    }
};

} // namespace sfcop::hle::user32::test
