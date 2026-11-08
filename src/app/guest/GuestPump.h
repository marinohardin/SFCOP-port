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
// Two kinds of turn read the window. A message turn (Pump) runs when the game peeks or gets a
// message and finds its queue empty. A turn inside a call (PumpInsideCall) runs from the rows of
// the imports the game calls, so that a load that does not peek for seconds still has its
// window's events read: macOS shows the busy cursor after about two seconds without. A turn
// inside a call only queues: the game receives what it read when it next peeks, in the order the
// window reported it, and it is never a frame.
//
// No lifted code in this file: it is built into the tests too.
#pragma once

#include "platform/Window/Window.h"

#include <cstdint>
#include <functional>
#include <thread>

namespace sfcop::guest {

class EventPump {
public:
    // Milliseconds on a monotonic clock. Empty: the host's steady clock. The tests give their own.
    using MonotonicMs = std::function<std::uint64_t()>;

    // `onEvent` runs, on the bound thread, for each event the window reports.
    EventPump(platform::Window &window, std::function<void(const platform::WindowEvent &)> onEvent,
              MonotonicMs now = {});

    // Makes the calling thread the one that pumps. Once, from Run(). The time between turns is
    // counted from here.
    void BindToCallingThread();

    // A message turn: reads the window's pending events and handles them. Returns false, having
    // done nothing, when the calling thread is not the bound one or none is bound.
    bool Pump();

    // The frame rule of the fixed-step clock (Options::onFrame): true when the bound thread's
    // last Pump() read no event and no turn inside a call read one since the Pump() before it
    // (another thread's call changes nothing; read it on the bound thread). That is the turn
    // after which the game's PeekMessageA finds its queue empty and the game runs a frame. A turn
    // that read an event does not count, so the events only a window gets (focus, exposure) leave
    // a windowed and a headless run alike; and a turn inside a call never counts, so the events
    // it read count for the next message turn, as they would have without it.
    bool FrameEnded() const { return frameEnded_; }

    // Turns on the turns inside calls: from then on PumpInsideCall() reads the window once at
    // least `intervalMs` have passed since the last turn of either kind. Off until called.
    void EnableInsideCalls(std::uint32_t intervalMs);

    // A turn inside a call the game made, from the row of an import: on the bound thread, with
    // the turns inside calls on and their interval passed, reads the window's pending events and
    // hands them on as Pump() does. Changes no frame. Returns whether it read the window.
    bool PumpInsideCall();

    // The longest time the bound thread went without a turn, measured at the start of each
    // turn: the gap.
    struct Gap {
        std::uint64_t endMs = 0;    // when the turn that ended it began, since BindToCallingThread()
        std::uint64_t lengthMs = 0; // since the turn before it (or the bind)
        bool insideCall = false;    // the turn that ended it was one inside a call
    };
    // Each gap of at least `thresholdMs` goes to `sink`, on the bound thread, before the turn
    // that ends it reads the window.
    void SetGapTrace(std::uint32_t thresholdMs, std::function<void(const Gap &)> sink);

    struct Counts {
        std::uint64_t messageTurns = 0;
        std::uint64_t insideCallTurns = 0;
        std::uint64_t eventsInsideCalls = 0; // events read by turns inside calls
        Gap longest;                         // the longest gap so far
    };
    const Counts &Totals() const { return counts_; }

private:
    bool onBoundThread() const;
    std::uint64_t now() const;
    void beginTurn(std::uint64_t nowMs, bool insideCall);
    // Reads every pending event; how many.
    std::uint64_t drain();

    platform::Window &window_;
    std::function<void(const platform::WindowEvent &)> onEvent_;
    MonotonicMs now_;
    std::thread::id mainThread_{};
    bool bound_ = false;
    bool frameEnded_ = false;
    bool readInsideCall_ = false; // a turn inside a call read an event since the last Pump()
    bool insideCalls_ = false;
    std::uint32_t intervalMs_ = 0;
    std::uint64_t boundAtMs_ = 0;
    std::uint64_t lastTurnMs_ = 0;
    std::uint32_t gapThresholdMs_ = 0;
    std::function<void(const Gap &)> gapSink_;
    Counts counts_;
};

} // namespace sfcop::guest
