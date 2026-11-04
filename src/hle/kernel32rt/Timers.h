// Timers: WINMM's timeSetEvent and timeKillEvent, built from Threads and Clock as Threads.h
// describes: one thread per timer that waits on its stop event until the next due time, runs the
// callback, and takes the next due time from the previous one, so a periodic timer does not drift;
// after a stall of more than a period it fires one catch-up tick, not one call per missed period.
//
// The callback runs on the timer's thread. The shim's callback enters the guest (hle_call_guest),
// which waits for the guest machine, so a guest thread that kills a timer gives the machine up
// around Kill() (it waits for the timer's thread to end).
//
// Threading: any thread. Kill() from inside a timer's own callback stops it without waiting.
#pragma once

#include "platform/Clock/Clock.h"
#include "platform/Threads/Threads.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

namespace sfcop::hle::kernel32rt {

class Timers {
public:
    using Callback = std::function<void(std::uint32_t id)>;

    Timers(platform::Threads &threads, platform::Clock &clock);
    ~Timers();
    Timers(const Timers &) = delete;
    Timers &operator=(const Timers &) = delete;

    // timeSetEvent: runs `callback` after `delayMs`, then every `delayMs` if `periodic` (a period
    // of 0 runs every millisecond). The timer's id (never 0), or 0 if no thread could be started.
    std::uint32_t Start(std::uint32_t delayMs, bool periodic, Callback callback);
    // timeKillEvent: stops the timer and, unless called from its own callback, waits until its
    // callback has returned. false for an id that is not running, which includes a one-shot that
    // has already fired (it leaves the table when its callback returns).
    bool Kill(std::uint32_t id);

private:
    struct Timer;

    void Reap(const std::shared_ptr<Timer> &timer);

    platform::Threads &threads_;
    platform::Clock &clock_;
    std::mutex lock_;
    std::uint32_t nextId_ = 1;
    std::map<std::uint32_t, std::shared_ptr<Timer>> timers_;
};

} // namespace sfcop::hle::kernel32rt
