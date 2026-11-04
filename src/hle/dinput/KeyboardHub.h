// What a DirectInput object and its keyboard devices share: the Input and Threads components,
// one lock, and the pump that moves key events from Input's queue into every acquired device's
// buffer. Input has one queue, so one reader drains it for every device; each device keeps its
// own records from there on.
#pragma once

#include "platform/Input/Input.h"
#include "platform/Threads/Threads.h"

#include <mutex>
#include <vector>

namespace sfcop::hle::dinput {

class KeyboardDevice;

class KeyboardHub {
public:
    KeyboardHub(platform::Input &input, platform::Threads &threads)
        : input_(input), threads_(threads) {}
    KeyboardHub(const KeyboardHub &) = delete;
    KeyboardHub &operator=(const KeyboardHub &) = delete;

    platform::Input &input() { return input_; }
    platform::Threads &threads() { return threads_; }

    // Guards the device list and every device's state. Device methods take it; Pump() takes it.
    std::mutex &mutex() { return mutex_; }

    // A device joins at creation and leaves at destruction; the caller holds mutex().
    void attach(KeyboardDevice *device);
    void detach(KeyboardDevice *device);

    // Drains Input's queue: each key press and release that has a DIK_ code goes to every device
    // (a device keeps it only while acquired); auto-repeats, mouse events and keys without a
    // DIK_ code are dropped, as DirectInput reports changes of state only. An overflow of Input's
    // queue marks every device's buffer as overflowed. Then each device that saw a change
    // signals its notification event. A key a device reported down that Input now has up with no
    // release event (focus loss) is released in the device's buffer too. The caller holds mutex().
    void pumpLocked();

    // pumpLocked() under the lock.
    void pump();

private:
    platform::Input &input_;
    platform::Threads &threads_;
    std::mutex mutex_;
    std::vector<KeyboardDevice *> devices_;
    // The time and number of the last key event drained, for records made up after it.
    std::uint32_t lastTimeMs_ = 0;
    std::uint32_t lastSequence_ = 0;
};

} // namespace sfcop::hle::dinput
