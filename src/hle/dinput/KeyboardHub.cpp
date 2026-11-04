#include "hle/dinput/KeyboardHub.h"

#include "hle/dinput/KeyTable.h"
#include "hle/dinput/KeyboardDevice.h"

#include <algorithm>

namespace sfcop::hle::dinput {

void KeyboardHub::attach(KeyboardDevice *device) {
    devices_.push_back(device);
}

void KeyboardHub::detach(KeyboardDevice *device) {
    devices_.erase(std::remove(devices_.begin(), devices_.end(), device), devices_.end());
}

void KeyboardHub::pumpLocked() {
    for (;;) {
        platform::Result<std::optional<platform::InputEvent>> next = input_.PollEvent();
        if (!next.Ok() || !next.Value().has_value()) {
            break;
        }
        const platform::InputEvent &event = *next.Value();
        bool down = event.type == platform::InputEventType::kKeyDown;
        if ((!down && event.type != platform::InputEventType::kKeyUp) || event.repeat) {
            continue;
        }
        lastTimeMs_ = event.timeMs;
        lastSequence_ = event.sequence;
        std::uint8_t dik = dikFromUsage(event.key);
        if (dik == 0) {
            continue;
        }
        for (KeyboardDevice *device : devices_) {
            device->onKeyLocked(dik, down, event.timeMs, event.sequence);
        }
    }
    if (input_.TakeOverflow().ValueOr(false)) {
        for (KeyboardDevice *device : devices_) {
            device->onOverflowLocked();
        }
    }
    // A key that went up without an event (focus lost) is released at the time of the last event
    // seen, numbered after it.
    platform::Result<platform::KeyboardState> keyboard = input_.GetKeyboard();
    if (keyboard.Ok()) {
        for (KeyboardDevice *device : devices_) {
            device->releaseHeldLocked(keyboard.Value(), lastTimeMs_, lastSequence_);
        }
    }
    for (KeyboardDevice *device : devices_) {
        device->finishPumpLocked();
    }
}

void KeyboardHub::pump() {
    std::lock_guard<std::mutex> lock(mutex_);
    pumpLocked();
}

} // namespace sfcop::hle::dinput
