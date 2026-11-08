#include "hle/user32/MessageQueue.h"

#include "hle/user32/User32Defs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace sfcop::hle::user32 {

namespace {

constexpr std::uint32_t kVkF10 = 0x79;
constexpr std::uint32_t kAnyWindow = 0;
constexpr std::uint32_t kThreadMessages = 0xFFFFFFFFu; // PeekMessage's hWnd -1
constexpr std::uint32_t kAltContext = 1u << 29;
// A click that comes before its window is active waits for the activation. Eight is far more than
// a person makes in the moment between the platform's button and focus events (a double click is
// four); the cap only stops a window that never activates from growing the list.
constexpr std::size_t kMaxPendingClicks = 8;
// Windows' own limit on a thread's posted messages (USERPostMessageLimit's default): a post beyond
// it fails. Reached only if the game stopped reading its queue for good.
constexpr std::size_t kMaxPosted = 10000;
// --trace: the first events only, never the whole run. Enough for the first focus, a few dozen
// clicks and what the game polled between them.
constexpr std::size_t kMaxTraceLines = 400;

bool isButtonDown(std::uint32_t message) {
    return message == kWmLButtonDown || message == kWmRButtonDown || message == kWmMButtonDown;
}

bool isButtonUp(std::uint32_t message) {
    return message == kWmLButtonUp || message == kWmRButtonUp || message == kWmMButtonUp;
}

std::uint32_t buttonMask(std::uint32_t message) {
    switch (message) {
    case kWmLButtonDown:
    case kWmLButtonUp:
        return kMkLButton;
    case kWmRButtonDown:
    case kWmRButtonUp:
        return kMkRButton;
    default:
        return kMkMButton;
    }
}

std::uint32_t packPoint(std::int32_t x, std::int32_t y) {
    return (static_cast<std::uint32_t>(x) & 0xFFFFu) | (static_cast<std::uint32_t>(y) << 16);
}

std::int32_t scaled(std::int32_t value, std::uint32_t from, std::uint32_t to) {
    if (from == 0 || from == to) {
        return value;
    }
    return static_cast<std::int32_t>(static_cast<std::int64_t>(value) * to / from);
}

bool inRange(std::uint32_t message, std::uint32_t first, std::uint32_t last) {
    return (first == 0 && last == 0) || (message >= first && message <= last);
}

} // namespace

MessageQueue::MessageQueue(WindowManager &windows, std::function<std::uint32_t()> now)
    : windows_(windows), now_(std::move(now)) {}

void MessageQueue::trace(const char *line) {
    if (trace_ && traced_ < kMaxTraceLines) {
        ++traced_;
        trace_(line);
    }
}

// A platform event's line carries the queue's clock, so that a press and its release can be
// told apart from a click that arrived in one pump.
void MessageQueue::traceEvent(const char *kind) {
    if (trace_ && traced_ < kMaxTraceLines) {
        char line[64];
        std::snprintf(line, sizeof line, "%s at %u ms", kind, now_());
        trace(line);
    }
}

bool MessageQueue::Post(std::uint32_t hwnd, std::uint32_t message, std::uint32_t wParam,
                        std::uint32_t lParam) {
    if (posted_.size() >= kMaxPosted) {
        trace("posted queue full: message dropped"); // the host's own input too; bounded by the trace cap
        return false;
    }
    posted_.push_back(Message{hwnd, message, wParam, lParam, now_(), cursorX_, cursorY_});
    return true;
}

void MessageQueue::PostQuit(std::int32_t code) {
    quit_ = code;
}

// ---- platform events ----

void MessageQueue::Feed(const platform::WindowEvent &event, HostSize host) {
    using platform::WindowEventType;
    switch (event.type) {
    case WindowEventType::kCloseRequested:
        // As Windows does for the close box: the active window gets WM_CLOSE (the game's main
        // window; the first window it made is a debug window it never activates).
        if (const std::uint32_t target = windows_.Active() != 0 ? windows_.Active() : lastActive_) {
            Post(target, kWmClose, 0, 0);
        } else if (windows_.TopLevelWindows().empty()) {
            earlyClose_ = true; // remembered for the first window made (User32::CreateWindow)
        }
        break;
    case WindowEventType::kFocusGained:
        trace("event focus-gained");
        activation(true);
        break;
    case WindowEventType::kFocusLost:
        trace("event focus-lost");
        activation(false);
        break;
    case WindowEventType::kKeyDown:
        key(event.input, true);
        break;
    case WindowEventType::kKeyUp:
        key(event.input, false);
        break;
    case WindowEventType::kMouseMove:
        mouse(event.input, kWmMouseMove, host);
        break;
    case WindowEventType::kMouseButtonDown:
    case WindowEventType::kMouseButtonUp: {
        const bool pressed = event.type == WindowEventType::kMouseButtonDown;
        std::uint32_t message = 0;
        std::uint32_t vk = 0;
        switch (event.input.button) {
        case platform::MouseButton::kLeft:
            message = pressed ? kWmLButtonDown : kWmLButtonUp, vk = kVkLButton;
            break;
        case platform::MouseButton::kRight:
            message = pressed ? kWmRButtonDown : kWmRButtonUp, vk = kVkRButton;
            break;
        case platform::MouseButton::kMiddle:
            message = pressed ? kWmMButtonDown : kWmMButtonUp, vk = kVkMButton;
            break;
        default:
            return; // the extra buttons: no message the game reads
        }
        traceEvent(pressed ? "event button-down" : "event button-up");
        setKey(vk, pressed);
        mouse(event.input, message, host);
        break;
    }
    case WindowEventType::kMouseWheel: {
        mouse(event.input, kWmNull, host); // the cursor only
        const std::uint32_t target = keyTarget();
        if (target != 0 && event.input.wheel != 0.0f) {
            const auto delta = static_cast<std::int32_t>(std::lround(event.input.wheel * 120.0f));
            Post(target, kWmMouseWheel,
                 mouseKeys() | static_cast<std::uint32_t>(delta) << 16, packPoint(cursorX_, cursorY_));
        }
        break;
    }
    default:
        // kText: TranslateMessage makes the characters from the keys, as the game expects;
        // kResized, kMinimized, kRestored: the window's size follows DirectDraw (WindowManager).
        break;
    }
}

void MessageQueue::activation(bool gained) {
    if (gained) {
        std::uint32_t target = windows_.Active();
        if (target != 0) {
            // ShowWindow already activated it: the platform's first focus event comes after the
            // window is up. Announcing the activation again would hand the game a second
            // WM_ACTIVATEAPP/WM_ACTIVATE/WM_SETFOCUS after a click that came before it.
            trace("  focus-gained: a window is already active, nothing announced");
            return;
        }
        if (windows_.IsWindow(lastActive_)) {
            target = lastActive_;
        }
        if (target == 0) {
            return; // no window yet: the first ShowWindow activates
        }
        // When a button is what activates the window, Windows sends WM_MOUSEACTIVATE first (the
        // default answer, MA_ACTIVATE, lets it through), then WM_ACTIVATE with WA_CLICKACTIVE,
        // then the button. The platform reports the click either side of the focus event, so a
        // held button down stands for the click that activates.
        const auto clicked = std::find_if(pending_.begin(), pending_.end(), [](const PendingClick &c) {
            return isButtonDown(c.message);
        });
        const bool byClick = clicked != pending_.end();
        if (byClick) {
            Post(target, kWmMouseActivate, windows_.TopLevelOf(target),
                 kHtClient | (clicked->message << 16));
        }
        windows_.SetActive(target);
        Post(target, kWmActivateApp, 1, 0);
        Post(target, kWmActivate, byClick ? kWaClickActive : kWaActive, 0);
        windows_.SetFocus(target);
        Post(target, kWmSetFocus, 0, 0);
        trace(byClick ? "  focus-gained: window activated by the held click, click replayed"
                      : "  focus-gained: window activated");
        for (const PendingClick &click : pending_) {
            if (isButtonDown(click.message)) {
                // Windows delivers the move to the click's position ahead of the button.
                postMouse(target, kWmMouseMove, click.keys & ~buttonMask(click.message), click.x,
                          click.y);
            }
            postMouse(target, click.message, click.keys, click.x, click.y);
        }
        pending_.clear();
        return;
    }
    // A click held for an activation that did not come is not for the next one.
    pending_.clear();
    const std::uint32_t active = windows_.Active();
    if (active == 0) {
        return;
    }
    lastActive_ = active;
    Post(active, kWmActivate, kWaInactive, 0);
    Post(active, kWmActivateApp, 0, 0);
    if (windows_.Focus() != 0) {
        Post(windows_.Focus(), kWmKillFocus, 0, 0);
    }
    windows_.SetFocus(0);
    windows_.SetActive(0);
    // Windows forgets the keys held when the application loses the keyboard.
    for (std::uint8_t &state : keys_) {
        state &= 0x01;
    }
}

void MessageQueue::key(const platform::InputEvent &input, bool isDown) {
    const std::optional<KeyCode> code = keyFromUsage(input.key);
    if (!code) {
        return;
    }
    setKey(code->vk, isDown);
    if (code->sideVk != code->vk) {
        setKey(code->sideVk, isDown);
    }
    const std::uint32_t target = keyTarget();
    if (target == 0) {
        return;
    }
    const bool alt = down(kVkMenu);
    const bool system = (alt && !down(kVkControl)) || code->vk == kVkF10 || code->vk == kVkMenu;
    std::uint32_t lParam = keyLParam(*code, isDown, input.repeat);
    if (alt) {
        lParam |= kAltContext;
    }
    const std::uint32_t message =
        isDown ? (system ? kWmSysKeyDown : kWmKeyDown) : (system ? kWmSysKeyUp : kWmKeyUp);
    Post(target, message, code->vk, lParam);
}

void MessageQueue::mouse(const platform::InputEvent &input, std::uint32_t message, HostSize host) {
    const gdi32::ScreenInfo screen = windows_.Screen();
    cursorX_ = scaled(input.x, host.width, static_cast<std::uint32_t>(screen.width));
    cursorY_ = scaled(input.y, host.height, static_cast<std::uint32_t>(screen.height));
    cursorX_ = std::clamp(cursorX_, 0, std::max(screen.width - 1, 0));
    cursorY_ = std::clamp(cursorY_, 0, std::max(screen.height - 1, 0));
    if (message == kWmNull) {
        return;
    }
    const std::uint32_t target = capture_ != 0 ? capture_ : windows_.Active();
    if (target == 0) {
        // A click into a window that is not active yet: some platforms report the button before
        // the focus. Windows activates the window and then delivers the click, so the button is
        // kept (its position and key state as it was) and posted once the window is active.
        // A button-up is kept only for a held down of the same button; the moves are dropped (the
        // cursor itself is already updated, and the click carries its own position).
        const bool up = message != kWmMouseMove && !isButtonDown(message);
        const bool heldDown =
            std::any_of(pending_.begin(), pending_.end(),
                        [message](const PendingClick &c) { return c.message + 1 == message; });
        if (message != kWmMouseMove && (!up || heldDown) && pending_.size() < kMaxPendingClicks) {
            pending_.push_back(PendingClick{message, mouseKeys(), cursorX_, cursorY_});
            trace("  button: no window active, held for the activation");
        } else {
            trace("  mouse: no window active, dropped");
        }
        return;
    }
    if (message != kWmMouseMove) {
        trace(isButtonDown(message) ? "  button-down posted" : "  button-up posted");
    }
    postMouse(target, message, mouseKeys(), cursorX_, cursorY_);
}

void MessageQueue::postMouse(std::uint32_t target, std::uint32_t message, std::uint32_t keys,
                             std::int32_t x, std::int32_t y) {
    std::int32_t originX = 0, originY = 0;
    windows_.ClientOrigin(target, originX, originY);
    const std::uint32_t point = packPoint(x - originX, y - originY);
    // As Windows does, a move that finds the game's last unread message to be a move to the same
    // window with the same buttons replaces it: the game reads where the mouse is, not every step
    // it took (a load that does not read its queue for seconds leaves one move, not thousands).
    // Buttons and keys are never merged or dropped: a click made during a load reaches the game
    // when it next reads its queue, as on Windows.
    if (message == kWmMouseMove && !posted_.empty()) {
        Message &last = posted_.back();
        if (last.message == kWmMouseMove && last.hwnd == target && last.wParam == keys) {
            last.lParam = point;
            last.time = now_();
            last.x = cursorX_;
            last.y = cursorY_;
            return;
        }
    }
    Post(target, message, keys, point);
}

void MessageQueue::setKey(std::uint32_t vk, bool isDown) {
    std::uint8_t &state = keys_[vk & 0xFF];
    if (isDown && (state & 0x80) == 0) {
        state ^= 0x01;
    }
    state = static_cast<std::uint8_t>((state & 0x01) | (isDown ? 0x80 : 0x00));
}

std::uint32_t MessageQueue::mouseKeys() const {
    std::uint32_t keys = 0;
    keys |= down(kVkLButton) ? kMkLButton : 0;
    keys |= down(kVkRButton) ? kMkRButton : 0;
    keys |= down(kVkMButton) ? kMkMButton : 0;
    keys |= down(kVkShift) ? kMkShift : 0;
    keys |= down(kVkControl) ? kMkControl : 0;
    return keys;
}

std::uint32_t MessageQueue::keyTarget() const {
    const std::uint32_t focus = windows_.Focus();
    if (focus != 0 && windows_.TopLevelOf(focus) == windows_.Active()) {
        return focus;
    }
    return windows_.Active();
}

std::uint16_t MessageQueue::KeyState(std::uint32_t vk) const {
    const std::uint8_t state = keys_[vk & 0xFF];
    return static_cast<std::uint16_t>(((state & 0x80) != 0 ? 0x8000 : 0) | (state & 0x01));
}

// ---- reading ----

std::optional<Message> MessageQueue::Peek(std::uint32_t hwnd, std::uint32_t first,
                                          std::uint32_t last, bool remove) {
    for (auto it = posted_.begin(); it != posted_.end(); ++it) {
        const bool forWindow = hwnd == kAnyWindow ||
                               (hwnd == kThreadMessages ? it->hwnd == 0
                                                        : it->hwnd == hwnd ||
                                                              windows_.TopLevelOf(it->hwnd) == hwnd);
        if (!forWindow || !inRange(it->message, first, last)) {
            continue;
        }
        const Message found = *it;
        if (remove) {
            posted_.erase(it);
            if (isButtonDown(found.message)) {
                trace("  peek: button-down taken");
            } else if (isButtonUp(found.message)) {
                trace("  peek: button-up taken");
            }
        }
        return found;
    }
    if (quit_ && (hwnd == kAnyWindow || hwnd == kThreadMessages) && inRange(kWmQuit, first, last)) {
        const Message quit{0, kWmQuit, static_cast<std::uint32_t>(*quit_), 0, now_(), cursorX_,
                           cursorY_};
        if (remove) {
            quit_.reset();
        }
        return quit;
    }
    if (hwnd != kThreadMessages && inRange(kWmPaint, first, last)) {
        // WM_PAINT stays while the window is invalid: BeginPaint or ValidateRect ends it.
        if (const std::uint32_t invalid = windows_.NextInvalid(hwnd)) {
            return Message{invalid, kWmPaint, 0, 0, now_(), cursorX_, cursorY_};
        }
    }
    return std::nullopt;
}

bool MessageQueue::Translate(const Message &message) {
    if (message.message != kWmKeyDown && message.message != kWmSysKeyDown) {
        return false;
    }
    const char c = charFromVk(message.wParam, down(kVkShift), down(kVkControl),
                              (keys_[kVkCapital] & 0x01) != 0);
    if (c == 0) {
        return false;
    }
    const std::uint32_t charMessage = message.message == kWmKeyDown ? kWmChar : kWmSysChar;
    posted_.push_front(Message{message.hwnd, charMessage, static_cast<std::uint8_t>(c),
                               message.lParam, message.time, message.x, message.y});
    return true;
}

void MessageQueue::Forget(std::uint32_t hwnd) {
    posted_.erase(std::remove_if(posted_.begin(), posted_.end(),
                                 [hwnd](const Message &m) { return m.hwnd == hwnd; }),
                  posted_.end());
    if (capture_ == hwnd) {
        capture_ = 0;
    }
    if (lastActive_ == hwnd) {
        lastActive_ = 0;
    }
}

} // namespace sfcop::hle::user32
