// An Input for tests: records each call and returns the values the test programs. PollEvent()
// hands out `events` front first. As a sink it appends delivered events to `events`, numbering
// them from 1, and on focus loss releases every key and button; WantsRelativeMouse() returns
// `relativeMode` (not recorded: it is const).
#pragma once

#include "platform/Core/MockLog.h"
#include "platform/Input/Input.h"

#include <deque>

namespace sfcop::platform {

class InputMock final : public Input {
public:
    MockLog log;

    std::deque<InputEvent> events;
    KeyboardState keyboard{};
    MouseState mouse;
    MouseMotion motion;
    bool overflow = false;
    bool relativeMode = false;
    std::uint32_t nextSequence = 1;
    Error failWith = Error::kNone; // when set, every Input method fails with it

    void OnInputEvent(const InputEvent &event) override {
        log.Record("OnInputEvent", event.type, event.key, event.timeMs);
        InputEvent numbered = event;
        numbered.sequence = nextSequence++;
        events.push_back(numbered);
    }
    void OnFocusChanged(bool focused) override {
        log.Record("OnFocusChanged", focused);
        if (!focused) {
            keyboard.fill(false);
            mouse.buttons.fill(false);
        }
    }
    bool WantsRelativeMouse() const override { return relativeMode; }

    Result<KeyboardState> GetKeyboard() override {
        log.Record("GetKeyboard");
        if (failWith != Error::kNone) {
            return failWith;
        }
        return keyboard;
    }
    Result<bool> IsKeyDown(Scancode key) override {
        log.Record("IsKeyDown", key);
        if (failWith != Error::kNone) {
            return failWith;
        }
        if (key >= kScancodeCount) {
            return Error::kInvalidArgument;
        }
        return keyboard[key];
    }
    Result<MouseState> GetMouse() override {
        log.Record("GetMouse");
        if (failWith != Error::kNone) {
            return failWith;
        }
        return mouse;
    }
    Result<MouseMotion> TakeMouseMotion() override {
        log.Record("TakeMouseMotion");
        if (failWith != Error::kNone) {
            return failWith;
        }
        MouseMotion taken = motion;
        motion = MouseMotion{};
        return taken;
    }
    Result<std::optional<InputEvent>> PollEvent() override {
        log.Record("PollEvent");
        if (failWith != Error::kNone) {
            return failWith;
        }
        if (events.empty()) {
            return std::optional<InputEvent>{};
        }
        InputEvent event = events.front();
        events.pop_front();
        return std::optional<InputEvent>{event};
    }
    Result<bool> TakeOverflow() override {
        log.Record("TakeOverflow");
        if (failWith != Error::kNone) {
            return failWith;
        }
        bool taken = overflow;
        overflow = false;
        return taken;
    }
    Status SetQueueCapacity(std::size_t capacity) override {
        log.Record("SetQueueCapacity", capacity);
        if (failWith != Error::kNone) {
            return failWith;
        }
        return {};
    }
    Status FlushEvents() override {
        log.Record("FlushEvents");
        if (failWith != Error::kNone) {
            return failWith;
        }
        events.clear();
        overflow = false;
        return {};
    }
    Status SetRelativeMode(bool enabled) override {
        log.Record("SetRelativeMode", enabled);
        if (failWith != Error::kNone) {
            return failWith;
        }
        relativeMode = enabled;
        return {};
    }
    Result<bool> IsRelativeMode() override {
        log.Record("IsRelativeMode");
        if (failWith != Error::kNone) {
            return failWith;
        }
        return relativeMode;
    }
};

} // namespace sfcop::platform
