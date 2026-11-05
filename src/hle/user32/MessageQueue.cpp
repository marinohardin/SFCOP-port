#include "hle/user32/MessageQueue.h"

#include "hle/user32/User32Defs.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace sfcop::hle::user32 {

namespace {

constexpr std::uint32_t kVkF10 = 0x79;
constexpr std::uint32_t kAnyWindow = 0;
constexpr std::uint32_t kThreadMessages = 0xFFFFFFFFu; // PeekMessage's hWnd -1
constexpr std::uint32_t kAltContext = 1u << 29;

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

void MessageQueue::Post(std::uint32_t hwnd, std::uint32_t message, std::uint32_t wParam,
                        std::uint32_t lParam) {
    posted_.push_back(Message{hwnd, message, wParam, lParam, now_(), cursorX_, cursorY_});
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
        activation(true);
        break;
    case WindowEventType::kFocusLost:
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
        if (target == 0 && windows_.IsWindow(lastActive_)) {
            target = lastActive_;
        }
        if (target == 0) {
            return; // no window yet: the first ShowWindow activates
        }
        windows_.SetActive(target);
        Post(target, kWmActivateApp, 1, 0);
        Post(target, kWmActivate, kWaActive, 0);
        windows_.SetFocus(target);
        Post(target, kWmSetFocus, 0, 0);
        return;
    }
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
        return;
    }
    std::int32_t originX = 0, originY = 0;
    windows_.ClientOrigin(target, originX, originY);
    Post(target, message, mouseKeys(), packPoint(cursorX_ - originX, cursorY_ - originY));
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
