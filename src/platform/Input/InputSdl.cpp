// The Input backend for the SDL3 build (DESIGN §5.7): an InputSink fed by the SDL3 window's
// PumpEvents(), keeping the keyboard and mouse state, the motion accumulators and the bounded
// event queue Input.h describes.
//
// Keys need no translation table here: Input.h names keys by USB HID keyboard usage ID, and SDL3
// defines SDL_Scancode as exactly those IDs (SDL_SCANCODE_A is 4, SDL_SCANCODE_RETURN 0x28,
// SDL_SCANCODE_LCTRL 0xE0), so a window passes an SDL_Scancode below kScancodeCount through as
// it is. InputSdlTest.cpp checks that identity against SDL3's own header for every key the
// DirectInput module maps. SDL3's codes from 256 up (media and mode keys) have no usage ID on
// page 0x07 and are dropped by the window or, if delivered, ignored for state and still queued.
//
// One mutex guards everything: the sink calls come from the main thread and the reads from any.
#include "platform/Input/Input.h"

#include <deque>
#include <mutex>

namespace sfcop::platform {
namespace {

constexpr std::size_t kDefaultQueueCapacity = 64;
// The most events SetQueueCapacity() accepts; more is kOutOfResources. DirectInput buffers are a
// few hundred records at most, and the queue grows only as events arrive.
constexpr std::size_t kMaxQueueCapacity = 65536;

class InputSdl final : public Input {
public:
    void OnInputEvent(const InputEvent &event) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ApplyToState(event);
        if (queue_.size() >= capacity_) {
            overflow_ = true;
            return;
        }
        InputEvent numbered = event;
        numbered.sequence = nextSequence_++;
        queue_.push_back(numbered);
    }

    void OnFocusChanged(bool focused) override {
        std::lock_guard<std::mutex> lock(mutex_);
        focused_ = focused;
        if (!focused) {
            keys_.fill(false);
            mouse_.buttons.fill(false);
        }
    }

    bool WantsRelativeMouse() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return relative_;
    }

    Result<KeyboardState> GetKeyboard() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!focused_) {
            return KeyboardState{};
        }
        return keys_;
    }

    Result<bool> IsKeyDown(Scancode key) override {
        if (key >= kScancodeCount) {
            return Error::kInvalidArgument;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        return focused_ && keys_[key];
    }

    Result<MouseState> GetMouse() override {
        std::lock_guard<std::mutex> lock(mutex_);
        MouseState state = mouse_;
        if (!focused_) {
            state.buttons.fill(false);
        }
        return state;
    }

    Result<MouseMotion> TakeMouseMotion() override {
        std::lock_guard<std::mutex> lock(mutex_);
        MouseMotion taken = motion_;
        motion_ = MouseMotion{};
        return taken;
    }

    Result<std::optional<InputEvent>> PollEvent() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return std::optional<InputEvent>{};
        }
        InputEvent event = queue_.front();
        queue_.pop_front();
        return std::optional<InputEvent>{event};
    }

    Result<bool> TakeOverflow() override {
        std::lock_guard<std::mutex> lock(mutex_);
        bool taken = overflow_;
        overflow_ = false;
        return taken;
    }

    Status SetQueueCapacity(std::size_t events) override {
        if (events == 0) {
            return Error::kInvalidArgument;
        }
        if (events > kMaxQueueCapacity) {
            return Error::kOutOfResources;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        capacity_ = events;
        queue_.clear();
        return {};
    }

    Status FlushEvents() override {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        overflow_ = false;
        return {};
    }

    Status SetRelativeMode(bool enabled) override {
        std::lock_guard<std::mutex> lock(mutex_);
        relative_ = enabled;
        return {};
    }

    Result<bool> IsRelativeMode() override {
        std::lock_guard<std::mutex> lock(mutex_);
        return relative_;
    }

private:
    // The immediate state an event leaves behind; the caller holds the mutex.
    void ApplyToState(const InputEvent &event) {
        switch (event.type) {
        case InputEventType::kKeyDown:
        case InputEventType::kKeyUp:
            // Unfocused, the window does not own the keyboard: a change would read as a press
            // nobody made once the focus returns.
            if (focused_ && event.key < kScancodeCount) {
                keys_[event.key] = event.type == InputEventType::kKeyDown;
            }
            break;
        case InputEventType::kMouseMove:
            MovePointer(event);
            motion_.dx += event.dx;
            motion_.dy += event.dy;
            break;
        case InputEventType::kMouseButtonDown:
        case InputEventType::kMouseButtonUp: {
            auto index = static_cast<std::size_t>(event.button);
            if (focused_ && index < kMouseButtonCount) {
                mouse_.buttons[index] = event.type == InputEventType::kMouseButtonDown;
            }
            MovePointer(event);
            break;
        }
        case InputEventType::kMouseWheel:
            motion_.wheel += event.wheel;
            break;
        }
    }

    // In relative mode the position stays where it was when the mode began (Input.h).
    void MovePointer(const InputEvent &event) {
        if (!relative_) {
            mouse_.x = event.x;
            mouse_.y = event.y;
        }
    }

    mutable std::mutex mutex_;
    KeyboardState keys_{};
    MouseState mouse_;
    MouseMotion motion_;
    std::deque<InputEvent> queue_;
    std::size_t capacity_ = kDefaultQueueCapacity;
    bool overflow_ = false;
    bool focused_ = true;
    bool relative_ = false;
    std::uint32_t nextSequence_ = 1;
};

} // namespace

std::unique_ptr<Input> makeInput() {
    return std::make_unique<InputSdl>();
}

} // namespace sfcop::platform
