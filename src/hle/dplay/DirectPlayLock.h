// The object's lock, private to DirectPlay.cpp and DirectPlayNet.cpp. Releasing it signals the
// events queued while it was held, so that Environment::signal never runs under the lock.
#pragma once

#include "hle/dplay/DirectPlay.h"

#include <mutex>
#include <utility>
#include <vector>

namespace sfcop::hle::dplay {

class DirectPlay::Lock {
public:
    explicit Lock(DirectPlay &owner) : owner_(owner), lock_(owner.mutex_) {}
    ~Lock() { Unlock(); }
    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;

    // Releases the lock (if held), then signals what was queued.
    void Unlock() {
        if (!lock_.owns_lock()) {
            return;
        }
        std::vector<std::uint32_t> events;
        events.swap(owner_.toSignal_);
        lock_.unlock();
        if (owner_.env_.signal) {
            for (std::uint32_t event : events) {
                owner_.env_.signal(event);
            }
        }
    }
    void Relock() { lock_.lock(); }
    // Releases the lock, waits through Environment::wait, and takes it again.
    void Wait(std::uint32_t milliseconds) {
        Unlock();
        owner_.waitFor(milliseconds);
        Relock();
    }

private:
    DirectPlay &owner_;
    std::unique_lock<std::mutex> lock_;
};

} // namespace sfcop::hle::dplay
