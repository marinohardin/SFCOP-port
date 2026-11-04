// Window: the game's one top-level window and the operating system's event loop (DESIGN §4.7,
// §5.6, §6.2). The USER32 HLE module turns its events into the window messages the game's window
// procedure receives; DirectDraw's display-mode calls become SetMode(). Drawing and presenting
// belong to Gpu, which is attached to this window; the frame cap is stored here and applied by
// Gpu::Present().
//
// PumpEvents() is the only call that reads the operating system's event queue. It fills this
// component's event queue and, from the same events, delivers key, mouse and focus events to the
// InputSink attached with AttachInput() (an Input of any backend). The window owns nothing of the
// sink's state; without a sink, input reaches only this component's queue.
//
// Threading: Create, Destroy, SetMode, SetTitle, SetCursorVisible, AttachInput, DetachInput and
// PumpEvents run on the main thread only (macOS requires it). The other methods may be called
// from any thread.
#pragma once

#include "platform/Core/Result.h"
#include "platform/Input/Input.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sfcop::platform {

enum class WindowMode : std::uint8_t {
    kWindowed,   // a decorated window of the requested size
    kFullscreen, // covers one display; whether the display's own mode changes is the backend's
                 // choice (DESIGN §5.6 allows rendering at the display's native resolution)
};

struct WindowDesc {
    std::string title;                      // UTF-8
    WindowMode mode = WindowMode::kWindowed;
    std::uint32_t width = 640;              // logical size, in the game's pixels
    std::uint32_t height = 480;
    bool resizable = true;                  // windowed only: the user may drag the frame
};

struct DisplayMode {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t refreshHz = 0; // 0 if the display does not say
};

struct WindowState {
    WindowMode mode = WindowMode::kWindowed;
    std::uint32_t width = 0;         // logical size the game sees
    std::uint32_t height = 0;
    std::uint32_t drawableWidth = 0; // size in physical pixels (larger on high-density displays)
    std::uint32_t drawableHeight = 0;
    bool focused = false;
    bool minimized = false;
};

// The frame cap Gpu::Present() honours. DESIGN §5.6: the default is the display's refresh rate;
// the original's uncapped loop is not kept, so there is no "uncapped" value.
struct FrameCap {
    std::uint32_t maxFramesPerSecond = 0; // 0 = the display's refresh rate
};

enum class WindowEventType : std::uint8_t {
    kCloseRequested,   // the user asked to close the window; nothing is closed yet
    kFocusGained,
    kFocusLost,
    kResized,          // `width`,`height` is the new logical size
    kMinimized,
    kRestored,
    kKeyDown,          // `input` holds the key event, as also queued in Input
    kKeyUp,
    kText,             // `codepoint` is one Unicode character typed (after the keyboard layout)
    kMouseMove,        // `input` holds the mouse event, as also queued in Input
    kMouseButtonDown,
    kMouseButtonUp,
    kMouseWheel,
};

struct WindowEvent {
    WindowEventType type = WindowEventType::kCloseRequested;
    std::uint32_t timeMs = 0;   // Clock::Ticks() when the operating system saw the event
    std::uint32_t width = 0;    // kResized
    std::uint32_t height = 0;
    char32_t codepoint = 0;     // kText
    InputEvent input;           // key and mouse events
};

class Window {
public:
    virtual ~Window() = default;

    // Opens the window and shows it. One window per Window object. Errors: kUnsupported;
    // kWrongState if it is already open; kInvalidArgument if a size is 0 or over 16384;
    // kOutOfResources if the system refuses a window.
    virtual Status Create(const WindowDesc &desc) = 0;

    // Closes the window and discards queued events. Closing a window that is not open does
    // nothing and succeeds. The Gpu attached to it must be detached first. Errors: kUnsupported.
    virtual Status Destroy() = 0;

    // Switches between windowed and fullscreen and sets the logical size. The change is reported
    // as a kResized event once it has happened. Errors: kUnsupported; kWrongState if the window
    // is not open; kInvalidArgument as Create().
    virtual Status SetMode(WindowMode mode, std::uint32_t width, std::uint32_t height) = 0;

    // The current mode, sizes, focus and minimised state. Errors: kUnsupported; kWrongState if
    // the window is not open.
    virtual Result<WindowState> GetState() = 0;

    // The mode of the display the window is on (before it is open: the main display).
    // Errors: kUnsupported; kIoError if the display cannot be queried.
    virtual Result<DisplayMode> GetDesktopMode() = 0;

    // Every mode the display offers for fullscreen, largest first. Errors: kUnsupported;
    // kIoError as GetDesktopMode().
    virtual Result<std::vector<DisplayMode>> ListDisplayModes() = 0;

    // Sets the title bar text (UTF-8). Errors: kUnsupported; kWrongState if not open.
    virtual Status SetTitle(std::string_view title) = 0;

    // Shows or hides the system cursor over the window. Errors: kUnsupported; kWrongState if
    // not open.
    virtual Status SetCursorVisible(bool visible) = 0;

    // Sets the frame cap Gpu::Present() honours from its next call. Errors: kUnsupported;
    // kInvalidArgument if over 1000.
    virtual Status SetFrameCap(FrameCap cap) = 0;

    // The current frame cap (the default is the display's refresh rate). Errors: kUnsupported.
    virtual Result<FrameCap> GetFrameCap() = 0;

    // Makes `sink` the receiver of this window's key, mouse and focus events, replacing any
    // earlier one. The sink must outlive the attachment (DetachInput() or Destroy()). Errors:
    // kUnsupported.
    virtual Status AttachInput(InputSink &sink) = 0;

    // Stops delivering to the attached sink; with none attached it does nothing and succeeds. A
    // relative mouse mode the sink asked for is turned off. Errors: kUnsupported.
    virtual Status DetachInput() = 0;

    // Reads every event the operating system has for this process without waiting. First applies
    // the attached sink's WantsRelativeMouse(); then, for each event in order, queues a
    // WindowEvent here and delivers key and mouse events to the sink's OnInputEvent() and focus
    // changes to its OnFocusChanged(). Call once per frame. Errors: kUnsupported.
    virtual Status PumpEvents() = 0;

    // Removes and returns the oldest queued window event, or std::nullopt if none is queued.
    // Errors: kUnsupported.
    virtual Result<std::optional<WindowEvent>> PollEvent() = 0;
};

// A Window whose every method fails with kUnsupported.
std::unique_ptr<Window> makeWindowStub();

// The build's real backend, on SDL3 (B1); declared here so that the client needs no
// backend header, defined by the backend's own source file once it lands.
std::unique_ptr<Window> makeWindow();

} // namespace sfcop::platform
