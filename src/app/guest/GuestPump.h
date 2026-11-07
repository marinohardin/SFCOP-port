// GuestPump: the platform's events, pumped on the game's main thread only (DESIGN §7.3).
//
// The guest calls PeekMessage and GetMessage from whichever of its threads asks; the window's
// event queue may be read from the main thread only (SDL on macOS requires it, platform/Window).
// The pump records the thread that bound it and does nothing on any other.
//
// This is the one place the Window is drained. Each event it reads goes to one sink, in order: the
// user32 module's message queue (hle/user32), which turns keys, mouse, focus and the close box
// into the messages the game reads.
//
// No lifted code in this file: it is built into the tests too.
#pragma once

#include "platform/Window/Window.h"

#include <functional>
#include <thread>

namespace sfcop::guest {

class EventPump {
public:
    // `onEvent` runs, on the bound thread, for each event the window reports.
    EventPump(platform::Window &window, std::function<void(const platform::WindowEvent &)> onEvent);

    // Makes the calling thread the one that pumps. Once, from Run().
    void BindToCallingThread();

    // Reads the window's pending events and handles them. Returns false, having done nothing,
    // when the calling thread is not the bound one or none is bound.
    bool Pump();

    // The frame rule of the fixed-step clock (Options::onFrame): true when the bound thread's
    // last Pump() read no event (another thread's call changes nothing; read it on the bound
    // thread), the turn after which the game's PeekMessageA finds its
    // queue empty and the game runs a frame. A turn that read an event does not count, so the
    // events only a window gets (focus, exposure) leave a windowed and a headless run alike.
    bool FrameEnded() const { return frameEnded_; }

private:
    platform::Window &window_;
    std::function<void(const platform::WindowEvent &)> onEvent_;
    std::thread::id mainThread_{};
    bool bound_ = false;
    bool frameEnded_ = false;
};

} // namespace sfcop::guest
