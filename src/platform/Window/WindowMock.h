// A Window for tests: records each call and returns the values the test programs. PumpEvents()
// does what the contract says with `pendingEvents` in place of the operating system: reads the
// attached sink's relative-mode wish into `relativeMouse`, moves the events into the queue
// PollEvent() reads, and delivers key, mouse and focus events to the sink.
#pragma once

#include "platform/Core/MockLog.h"
#include "platform/Window/Window.h"

#include <deque>

namespace sfcop::platform {

class WindowMock final : public Window {
public:
    MockLog log;

    WindowDesc lastDesc;
    std::deque<WindowEvent> pendingEvents; // delivered by the next PumpEvents()
    std::deque<WindowEvent> queuedEvents;  // what PollEvent() hands out
    InputSink *sink = nullptr;             // the attached sink, if any
    bool relativeMouse = false;            // as last applied by PumpEvents()

    Status createResult;
    Status destroyResult;
    Status setModeResult;
    Result<WindowState> stateResult = WindowState{};
    Result<DisplayMode> desktopModeResult = DisplayMode{1920, 1080, 60};
    Result<std::vector<DisplayMode>> displayModesResult = std::vector<DisplayMode>{};
    Status setTitleResult;
    Status setCursorVisibleResult;
    Status setFrameCapResult;
    Result<FrameCap> frameCapResult = FrameCap{};
    Status pumpEventsResult;

    Status Create(const WindowDesc &desc) override {
        log.Record("Create", desc.title, desc.mode, desc.width, desc.height);
        lastDesc = desc;
        return createResult;
    }
    Status Destroy() override {
        log.Record("Destroy");
        return destroyResult;
    }
    Status SetMode(WindowMode mode, std::uint32_t width, std::uint32_t height) override {
        log.Record("SetMode", mode, width, height);
        return setModeResult;
    }
    Result<WindowState> GetState() override {
        log.Record("GetState");
        return stateResult;
    }
    Result<DisplayMode> GetDesktopMode() override {
        log.Record("GetDesktopMode");
        return desktopModeResult;
    }
    Result<std::vector<DisplayMode>> ListDisplayModes() override {
        log.Record("ListDisplayModes");
        return displayModesResult;
    }
    Status SetTitle(std::string_view title) override {
        log.Record("SetTitle", title);
        return setTitleResult;
    }
    Status SetCursorVisible(bool visible) override {
        log.Record("SetCursorVisible", visible);
        return setCursorVisibleResult;
    }
    Status SetFrameCap(FrameCap cap) override {
        log.Record("SetFrameCap", cap.maxFramesPerSecond);
        return setFrameCapResult;
    }
    Result<FrameCap> GetFrameCap() override {
        log.Record("GetFrameCap");
        return frameCapResult;
    }
    Status PumpEvents() override {
        log.Record("PumpEvents");
        if (!pumpEventsResult.Ok()) {
            return pumpEventsResult;
        }
        relativeMouse = sink != nullptr && sink->WantsRelativeMouse();
        while (!pendingEvents.empty()) {
            WindowEvent event = pendingEvents.front();
            pendingEvents.pop_front();
            queuedEvents.push_back(event);
            if (sink == nullptr) {
                continue;
            }
            switch (event.type) {
            case WindowEventType::kFocusGained:
                sink->OnFocusChanged(true);
                break;
            case WindowEventType::kFocusLost:
                sink->OnFocusChanged(false);
                break;
            case WindowEventType::kKeyDown:
            case WindowEventType::kKeyUp:
            case WindowEventType::kMouseMove:
            case WindowEventType::kMouseButtonDown:
            case WindowEventType::kMouseButtonUp:
            case WindowEventType::kMouseWheel:
                sink->OnInputEvent(event.input);
                break;
            default:
                break;
            }
        }
        return {};
    }
    Status AttachInput(InputSink &attached) override {
        log.Record("AttachInput");
        sink = &attached;
        return {};
    }
    Status DetachInput() override {
        log.Record("DetachInput");
        sink = nullptr;
        relativeMouse = false;
        return {};
    }
    Result<std::optional<WindowEvent>> PollEvent() override {
        log.Record("PollEvent");
        if (queuedEvents.empty()) {
            return std::optional<WindowEvent>{};
        }
        WindowEvent event = queuedEvents.front();
        queuedEvents.pop_front();
        return std::optional<WindowEvent>{event};
    }
};

} // namespace sfcop::platform
