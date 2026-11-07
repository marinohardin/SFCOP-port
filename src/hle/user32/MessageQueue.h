// MessageQueue: the game's thread message queue (DESIGN §4.7, §5.6). What the platform Window
// reports becomes the messages a Windows queue would hold: keys and characters, the mouse in the
// client coordinates of the window that gets it, focus as activation, the close box as
// WM_CLOSE. PeekMessageA and GetMessageA take from it in Windows' order: posted messages, then
// WM_QUIT, then WM_PAINT for a window with an invalid region.
//
// Keys go to the focus window, or the active window when nothing has the focus; the mouse goes
// to the window that captured it, or the active window. The game's own windows are top-level
// windows that cover the screen, so no hit test is made below them.
//
// GetKeyState answers from the state the queue has seen (as Windows answers from the messages
// read so far); GetAsyncKeyState from the Input component, now (User32).
//
// Not thread-safe: the guest layer calls it under the machine lock.
#pragma once

#include "hle/user32/Keys.h"
#include "hle/user32/WindowManager.h"
#include "platform/Window/Window.h"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace sfcop::hle::user32 {

struct Message {
    std::uint32_t hwnd = 0;
    std::uint32_t message = 0;
    std::uint32_t wParam = 0;
    std::uint32_t lParam = 0;
    std::uint32_t time = 0;
    std::int32_t x = 0; // the cursor on the screen when the message was posted
    std::int32_t y = 0;
};

// The host window's logical size, against which the platform reports the mouse.
struct HostSize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

class MessageQueue {
public:
    // `now` is the message time (GetTickCount's clock).
    MessageQueue(WindowManager &windows, std::function<std::uint32_t()> now);

    void Post(std::uint32_t hwnd, std::uint32_t message, std::uint32_t wParam,
              std::uint32_t lParam);
    // PostQuitMessage: WM_QUIT is returned once the posted messages are read.
    void PostQuit(std::int32_t code);
    bool QuitPosted() const { return quit_.has_value(); }

    // The messages a platform event makes. `host` is the window's logical size now (0 for
    // unknown: the screen's own size).
    void Feed(const platform::WindowEvent &event, HostSize host);

    // PeekMessageA's search: the first message for `hwnd` (0: any; 0xFFFFFFFF: posted with no
    // window) whose number lies in [first, last] (both 0: any), removed if `remove`.
    std::optional<Message> Peek(std::uint32_t hwnd, std::uint32_t first, std::uint32_t last,
                                bool remove);

    // TranslateMessageA: queues the WM_CHAR (WM_SYSCHAR) a key-down types, ahead of the rest.
    // Whether it queued one.
    bool Translate(const Message &message);

    // Whether a close request came before any window could take it (and clears that): the first
    // window made gets the WM_CLOSE.
    bool TakeEarlyClose() { return std::exchange(earlyClose_, false); }

    // Drops the messages of a window being destroyed.
    void Forget(std::uint32_t hwnd);

    // GetKeyState's answer for a virtual key: bit 15 down, bit 0 toggled.
    std::uint16_t KeyState(std::uint32_t vk) const;
    // The cursor on the screen, as the last mouse event left it.
    void Cursor(std::int32_t &x, std::int32_t &y) const {
        x = cursorX_;
        y = cursorY_;
    }
    void SetCursor(std::int32_t x, std::int32_t y) {
        cursorX_ = x;
        cursorY_ = y;
    }

    // SetCapture and ReleaseCapture; the previous capture.
    std::uint32_t SetCapture(std::uint32_t hwnd) { return std::exchange(capture_, hwnd); }
    std::uint32_t Capture() const { return capture_; }

    // ClearPending: the clicks held for an activation that came another way (ShowWindow).
    void ClearPending() { pending_.clear(); }

    // --trace: one line for each of the first platform focus and button events, the queue's
    // reaction to it, the button messages PeekMessageA hands out and the polled calls' answers
    // (kinds and order only: no positions, no text). Unset: nothing is traced.
    void SetTrace(std::function<void(const char *)> trace) { trace_ = std::move(trace); }
    // A line from the rows that poll the mouse (User32), under the same cap.
    void Trace(const char *line) { trace(line); }

    std::size_t Size() const { return posted_.size(); }

private:
    void key(const platform::InputEvent &input, bool down);
    void mouse(const platform::InputEvent &input, std::uint32_t message, HostSize host);
    void postMouse(std::uint32_t target, std::uint32_t message, std::uint32_t keys,
                   std::int32_t x, std::int32_t y);
    void activation(bool gained);
    void trace(const char *line);
    void traceEvent(const char *kind);
    void setKey(std::uint32_t vk, bool down);
    bool down(std::uint32_t vk) const { return (keys_[vk & 0xFF] & 0x80) != 0; }
    std::uint32_t mouseKeys() const;
    std::uint32_t keyTarget() const;

    struct PendingClick {
        std::uint32_t message, keys;
        std::int32_t x, y;
    };
    std::vector<PendingClick> pending_; // buttons that came before any window was active

    std::function<void(const char *)> trace_;
    std::size_t traced_ = 0;

    WindowManager &windows_;
    std::function<std::uint32_t()> now_;
    std::deque<Message> posted_;
    std::optional<std::int32_t> quit_;
    std::array<std::uint8_t, 256> keys_{}; // bit 7 down, bit 0 toggled
    std::int32_t cursorX_ = 0, cursorY_ = 0;
    std::uint32_t capture_ = 0;
    bool earlyClose_ = false;      // a close request with no window yet
    std::uint32_t lastActive_ = 0; // the window that loses and regains activation with the app
};

} // namespace sfcop::hle::user32
