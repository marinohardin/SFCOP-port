#include "app/input/ScriptedWindow.h"

#include <utility>

namespace sfcop::app::input {

using platform::Error;
using platform::Result;
using platform::Status;
using platform::WindowEvent;
using platform::WindowEventType;

ScriptedWindow::ScriptedWindow(platform::Window &inner, ScriptPlayer &player,
                               platform::Clock &clock,
                               std::function<void(const PlayerFailure &)> onFailure)
    : inner_(inner), player_(player), clock_(clock), onFailure_(std::move(onFailure)) {}

void ScriptedWindow::BindToCallingThread() {
    thread_ = std::this_thread::get_id();
    bound_ = true;
}

Status ScriptedWindow::Create(const platform::WindowDesc &desc) {
    return inner_.Create(desc);
}

Status ScriptedWindow::Destroy() {
    scripted_.clear();
    return inner_.Destroy();
}

Status ScriptedWindow::SetMode(platform::WindowMode mode, std::uint32_t width,
                               std::uint32_t height) {
    return inner_.SetMode(mode, width, height);
}

Result<platform::WindowState> ScriptedWindow::GetState() {
    return inner_.GetState();
}

Result<platform::DisplayMode> ScriptedWindow::GetDesktopMode() {
    return inner_.GetDesktopMode();
}

Result<std::vector<platform::DisplayMode>> ScriptedWindow::ListDisplayModes() {
    return inner_.ListDisplayModes();
}

Status ScriptedWindow::SetTitle(std::string_view title) {
    return inner_.SetTitle(title);
}

Status ScriptedWindow::SetCursorVisible(bool visible) {
    return inner_.SetCursorVisible(visible);
}

Status ScriptedWindow::SetFrameCap(platform::FrameCap cap) {
    return inner_.SetFrameCap(cap);
}

Result<platform::FrameCap> ScriptedWindow::GetFrameCap() {
    return inner_.GetFrameCap();
}

Status ScriptedWindow::AttachInput(platform::InputSink &sink) {
    sink_ = &sink;
    return inner_.AttachInput(sink);
}

Status ScriptedWindow::DetachInput() {
    sink_ = nullptr;
    return inner_.DetachInput();
}

void ScriptedWindow::deliver(const WindowEvent &event) {
    scripted_.push_back(event);
    if (sink_ == nullptr) {
        return;
    }
    switch (event.type) {
    case WindowEventType::kFocusGained:
        sink_->OnFocusChanged(true);
        break;
    case WindowEventType::kKeyDown:
    case WindowEventType::kKeyUp:
    case WindowEventType::kMouseMove:
    case WindowEventType::kMouseButtonDown:
    case WindowEventType::kMouseButtonUp:
        sink_->OnInputEvent(event.input);
        break;
    default:
        break;
    }
}

Status ScriptedWindow::PumpEvents() {
    if (!bound_ || std::this_thread::get_id() != thread_) {
        return Error::kWrongState;
    }
    const Status inner = inner_.PumpEvents();
    ++frame_;
    const auto ticks = clock_.Ticks();
    const std::uint32_t now = ticks.Ok() ? ticks.Value() : 0;
    for (const WindowEvent &event : player_.Advance(frame_, now)) {
        // A click or key begins with the move or the key down: the window comes to the front.
        if (event.type == WindowEventType::kMouseMove || event.type == WindowEventType::kKeyDown) {
            const auto state = inner_.GetState();
            if (state.Ok() && !state.Value().focused) {
                WindowEvent focus;
                focus.type = WindowEventType::kFocusGained;
                focus.timeMs = now;
                deliver(focus);
            }
        }
        deliver(event);
    }
    if (player_.Failure() && !failureReported_) {
        failureReported_ = true;
        if (onFailure_) {
            onFailure_(*player_.Failure());
        }
    }
    // The headless stub refuses every call; the script plays on regardless.
    return inner.Ok() || inner.Code() == Error::kUnsupported ? Status{} : inner;
}

Result<std::optional<WindowEvent>> ScriptedWindow::PollEvent() {
    const auto real = inner_.PollEvent();
    if (real.Ok() && real.Value()) {
        return real;
    }
    if (!scripted_.empty()) {
        WindowEvent event = scripted_.front();
        scripted_.pop_front();
        return std::optional<WindowEvent>{event};
    }
    if (!real.Ok() && real.Code() != Error::kUnsupported) {
        return real.Code();
    }
    return std::optional<WindowEvent>{};
}

} // namespace sfcop::app::input
