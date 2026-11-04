#include "GuestPump.h"

#include <utility>

namespace sfcop::guest {

EventPump::EventPump(platform::Window &window, std::function<void()> closeRequested)
    : window_(window), closeRequested_(std::move(closeRequested)) {}

void EventPump::BindToCallingThread() {
    mainThread_ = std::this_thread::get_id();
    bound_ = true;
}

bool EventPump::Pump() {
    if (!bound_ || std::this_thread::get_id() != mainThread_) {
        // Another guest thread asked: it may yield to the main thread, which pumps in its turn.
        std::this_thread::yield();
        return false;
    }
    static_cast<void>(window_.PumpEvents());
    for (;;) {
        auto event = window_.PollEvent();
        if (!event.Ok() || !event.Value()) {
            break;
        }
        if (event.Value()->type == platform::WindowEventType::kCloseRequested && closeRequested_) {
            closeRequested_();
        }
    }
    return true;
}

} // namespace sfcop::guest
