// Input: keyboard and mouse as the DirectInput HLE module needs them (DESIGN §4.7, §5.7, §6.2):
// immediate state (which keys are down, where the mouse is, motion since the last read) and a
// buffered queue of timestamped events. Keys are identified by USB HID keyboard usage IDs; the
// HLE module maps them to the game's key codes.
//
// Input never reads the operating system. Every Input is an InputSink: a Window it is attached to
// (Window::AttachInput) delivers key, mouse and focus events to it from Window::PumpEvents(), the
// one place that reads the operating system's events, and applies the relative mode Input asks
// for. So any Input backend works with any Window backend. Positions are in window client
// coordinates, in pixels of the window's logical size (Window::GetState().width/height).
//
// Threading: the InputSink methods are called on the main thread, from Window::PumpEvents().
// Every Input method may be called from any thread at the same time as those.
#pragma once

#include "platform/Core/Result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace sfcop::platform {

// A USB HID keyboard/keypad usage ID (usage page 0x07): 0x04 is A, 0x28 Return, 0xE0 left Ctrl.
using Scancode = std::uint16_t;
constexpr std::size_t kScancodeCount = 256;

enum class MouseButton : std::uint8_t { kLeft, kRight, kMiddle, kX1, kX2 };
constexpr std::size_t kMouseButtonCount = 5;

enum class InputEventType : std::uint8_t {
    kKeyDown,         // `key` went down (or auto-repeated, with `repeat` set)
    kKeyUp,           // `key` went up
    kMouseMove,       // the mouse moved to `x`,`y` by `dx`,`dy`
    kMouseButtonDown, // `button` went down at `x`,`y`
    kMouseButtonUp,   // `button` went up at `x`,`y`
    kMouseWheel,      // the wheel turned by `wheel` notches at `x`,`y`
};

struct InputEvent {
    InputEventType type = InputEventType::kKeyDown;
    std::uint32_t timeMs = 0;   // Clock::Ticks() when the operating system saw the event
    std::uint32_t sequence = 0; // increases by one per event, wrapping; orders equal timestamps
    Scancode key = 0;
    bool repeat = false;
    MouseButton button = MouseButton::kLeft;
    std::int32_t x = 0;  // position after the event
    std::int32_t y = 0;
    std::int32_t dx = 0; // motion; meaningful in relative mode, where x and y do not change
    std::int32_t dy = 0;
    float wheel = 0.0f; // notches; positive is away from the user
};

// One flag per Scancode: true while the key is down.
using KeyboardState = std::array<bool, kScancodeCount>;

struct MouseState {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::array<bool, kMouseButtonCount> buttons{}; // indexed by MouseButton
};

// Motion accumulated since the last TakeMouseMotion().
struct MouseMotion {
    std::int32_t dx = 0;
    std::int32_t dy = 0;
    float wheel = 0.0f; // notches, as InputEvent::wheel
};

// What a Window delivers keyboard and mouse input to (Window::AttachInput). These are
// notifications, not requests: they cannot fail, and a sink that cannot use one ignores it.
// Called on the main thread, from Window::PumpEvents(), in the order the operating system
// reported the events.
class InputSink {
public:
    virtual ~InputSink() = default;

    // One key or mouse event (type kKeyDown to kMouseWheel) with its timestamp and position. The
    // sink updates its state and appends the event to its queue, numbering it in `sequence`
    // itself (the delivered value is ignored); a full queue sets the overflow flag instead.
    virtual void OnInputEvent(const InputEvent &event) = 0;

    // The window gained or lost focus. On loss, every key and button reads up from then on; no
    // up events are queued for them.
    virtual void OnFocusChanged(bool focused) = 0;

    // Whether the sink wants relative mouse mode. The window reads it at the start of every
    // PumpEvents() and applies it (cursor hidden and held, motion unbounded) before delivering.
    virtual bool WantsRelativeMouse() const = 0;
};

// Every Input is the sink of the window it is attached to.
class Input : public InputSink {
public:
    virtual ~Input() = default;

    // Which keys are down now. Every key reads up while the window does not have focus. Errors:
    // kUnsupported.
    virtual Result<KeyboardState> GetKeyboard() = 0;

    // Whether one key is down now; as GetKeyboard()[key]. Errors: kUnsupported;
    // kInvalidArgument if `key` >= kScancodeCount.
    virtual Result<bool> IsKeyDown(Scancode key) = 0;

    // The mouse position and buttons now. In relative mode the position stays where it was when
    // the mode began. Errors: kUnsupported.
    virtual Result<MouseState> GetMouse() = 0;

    // The motion and wheel turn accumulated since the previous call (or since creation), then
    // resets the accumulator. Works in both modes. Errors: kUnsupported.
    virtual Result<MouseMotion> TakeMouseMotion() = 0;

    // Removes and returns the oldest buffered event, or std::nullopt if the queue is empty.
    // Errors: kUnsupported.
    virtual Result<std::optional<InputEvent>> PollEvent() = 0;

    // Whether events were dropped because the queue was full since the previous call; clears the
    // flag. When the queue is full the newest events are dropped, so the queue keeps the
    // earliest. Errors: kUnsupported.
    virtual Result<bool> TakeOverflow() = 0;

    // Sets how many events the queue holds (at least 1; the default is 64) and empties it.
    // Errors: kUnsupported; kInvalidArgument if `events` is 0; kOutOfResources.
    virtual Status SetQueueCapacity(std::size_t events) = 0;

    // Discards every buffered event and the overflow flag. Errors: kUnsupported.
    virtual Status FlushEvents() = 0;

    // In relative mode the cursor is hidden and held in the window, and motion is reported
    // without limit at the window edges (what the game's exclusive mouse expects). Off restores
    // the cursor where it was. Takes effect at the attached window's next PumpEvents(), through
    // WantsRelativeMouse(). Errors: kUnsupported.
    virtual Status SetRelativeMode(bool enabled) = 0;

    // Whether relative mode is on. Errors: kUnsupported.
    virtual Result<bool> IsRelativeMode() = 0;
};

// An Input whose every method fails with kUnsupported; as a sink it ignores every event and never
// wants relative mode.
std::unique_ptr<Input> makeInputStub();

// The build's real backend, on SDL3 (B3); declared here so that the client needs no
// backend header, defined by the backend's own source file once it lands.
std::unique_ptr<Input> makeInput();

} // namespace sfcop::platform
