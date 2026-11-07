#include "hle/kernel32rt/Timers.h"

#include "hle/kernel32rt/Kernel32rt.h"
#include "platform/Clock/FixedStepClock.h"

#include <atomic>
#include <utility>
#include <vector>

namespace sfcop::hle::kernel32rt {

struct Timers::Timer {
    std::uint32_t id = 0;
    platform::Waitable stop;
    platform::Waitable thread;
    std::atomic<std::uint32_t> threadId{0}; // Threads' id of the timer's thread, once it runs
    // On frames only (no thread): when it falls due, and what it runs.
    std::uint32_t due = 0;
    std::uint32_t period = 0;
    bool periodic = false;
    Callback callback;
};

namespace {

std::uint32_t ticksOf(platform::Clock &clock) {
    const auto ticks = clock.Ticks();
    return ticks.Ok() ? ticks.Value() : 0u;
}

} // namespace

Timers::Timers(platform::Threads &threads, platform::Clock &clock)
    : threads_(threads), clock_(clock),
      onFrames_(dynamic_cast<platform::FixedStepClock *>(&clock) != nullptr) {}

Timers::~Timers() {
    std::vector<std::uint32_t> ids;
    {
        std::lock_guard<std::mutex> hold(lock_);
        for (const auto &entry : timers_) {
            ids.push_back(entry.first);
        }
    }
    for (std::uint32_t id : ids) {
        Kill(id);
    }
}

std::uint32_t Timers::Start(std::uint32_t delayMs, bool periodic, Callback callback) {
    if (onFrames_) {
        return StartOnFrames(delayMs, periodic, std::move(callback));
    }
    auto stop = threads_.MakeEvent(true, false, {});
    if (!stop.Ok()) {
        return 0;
    }
    auto timer = std::make_shared<Timer>();
    timer->stop = stop.Value().handle;
    const std::uint32_t period = delayMs == 0 ? 1u : delayMs;
    const std::uint32_t firstDue = ticksOf(clock_) + delayMs;
    platform::Threads &threads = threads_;
    platform::Clock &clock = clock_;
    auto body = [this, timer, period, periodic, firstDue, callback = std::move(callback), &threads,
                 &clock]() -> std::uint32_t {
        const auto self = threads.CurrentThreadId();
        timer->threadId = self.Ok() ? self.Value() : 0u;
        std::uint32_t due = firstDue;
        for (;;) {
            const auto left = static_cast<std::int32_t>(due - ticksOf(clock));
            const platform::Waitable stop = timer->stop;
            const auto waited = threads.Wait(&stop, 1, platform::WaitMode::kAny,
                                             static_cast<std::uint32_t>(left > 0 ? left : 0));
            if (waited.Ok() && waited.Value().outcome == platform::WaitOutcome::kSignaled) {
                return 0; // Kill() stopped it, and took it out of the table and closes its handles
            }
            if (!waited.Ok() || waited.Value().outcome != platform::WaitOutcome::kTimedOut) {
                break;
            }
            callback(timer->id);
            if (!periodic) {
                break;
            }
            // Drift-free: the next due time follows the last one, not the clock. After a stall
            // of more than a period (the machine busy, the process suspended) one catch-up tick
            // is due at once and the cadence resumes from there, not a call per missed period.
            due += period;
            if (static_cast<std::int32_t>(ticksOf(clock) - due) >= static_cast<std::int32_t>(period)) {
                due = ticksOf(clock);
            }
        }
        Reap(timer);
        return 0;
    };
    platform::ThreadOptions options;
    options.stackBytes = kGuestThreadHostStackBytes; // the callback runs guest code
    // The timer is in the table before its thread runs, so a callback that kills its own id
    // finds it; the thread's first use of the table waits for this lock, and so for the handle.
    std::lock_guard<std::mutex> hold(lock_);
    timer->id = nextId_++;
    if (nextId_ == 0) {
        nextId_ = 1;
    }
    timers_[timer->id] = timer;
    auto started = threads_.StartThread(std::move(body), options);
    if (!started.Ok()) {
        timers_.erase(timer->id);
        static_cast<void>(threads_.Close(timer->stop));
        return 0;
    }
    timer->thread = started.Value();
    return timer->id;
}

// A timer whose thread has ended by itself (a one-shot that fired) leaves the table and gives its
// handles back; a timer Kill() already took out is Kill()'s to close.
void Timers::Reap(const std::shared_ptr<Timer> &timer) {
    {
        std::lock_guard<std::mutex> hold(lock_);
        const auto found = timers_.find(timer->id);
        if (found == timers_.end() || found->second != timer) {
            return;
        }
        timers_.erase(found);
    }
    static_cast<void>(threads_.Close(timer->thread));
    static_cast<void>(threads_.Close(timer->stop));
}

bool Timers::Kill(std::uint32_t id) {
    std::shared_ptr<Timer> timer;
    {
        std::lock_guard<std::mutex> hold(lock_);
        const auto found = timers_.find(id);
        if (found == timers_.end()) {
            return false;
        }
        timer = found->second;
        timers_.erase(found);
    }
    if (onFrames_) {
        // Out of the table, so FireDue() runs it no more; a callback FireDue() is running now
        // ends before this returns (from that callback itself, the lock is the caller's own).
        std::lock_guard<std::recursive_mutex> wait(firing_);
        return true;
    }
    // The timer's own handles: none of these can fail while the timer is in the table.
    static_cast<void>(threads_.SetEvent(timer->stop));
    const auto self = threads_.CurrentThreadId();
    const bool fromCallback = self.Ok() && self.Value() == timer->threadId.load();
    if (!fromCallback) {
        static_cast<void>(threads_.Wait(&timer->thread, 1, platform::WaitMode::kAny, std::nullopt));
    }
    static_cast<void>(threads_.Close(timer->thread));
    static_cast<void>(threads_.Close(timer->stop));
    return true;
}

std::uint32_t Timers::StartOnFrames(std::uint32_t delayMs, bool periodic, Callback callback) {
    auto timer = std::make_shared<Timer>();
    timer->period = delayMs == 0 ? 1u : delayMs;
    timer->due = ticksOf(clock_) + delayMs;
    timer->periodic = periodic;
    timer->callback = std::move(callback);
    std::lock_guard<std::mutex> hold(lock_);
    timer->id = nextId_++;
    if (nextId_ == 0) {
        nextId_ = 1;
    }
    timers_[timer->id] = timer;
    return timer->id;
}

void Timers::FireDue() {
    if (!onFrames_) {
        return;
    }
    // One clock reading for the whole call: every timer is judged at the same time, and a
    // callback that reads the clock does not make a later one due in this frame.
    const std::uint32_t now = ticksOf(clock_);
    std::lock_guard<std::recursive_mutex> firing(firing_);
    for (;;) {
        std::shared_ptr<Timer> next;
        {
            std::lock_guard<std::mutex> hold(lock_);
            for (const auto &entry : timers_) {
                const Timer &timer = *entry.second;
                if (static_cast<std::int32_t>(now - timer.due) < 0) {
                    continue;
                }
                // The earliest due time first; the map's order (by id) breaks a tie.
                if (!next || static_cast<std::int32_t>(timer.due - next->due) < 0) {
                    next = entry.second;
                }
            }
            if (!next) {
                return;
            }
            if (next->periodic) {
                // As the timer thread's rule: the next due time from the last, and after a
                // stall of more than a period one catch-up tick now, not a call per period.
                next->due += next->period;
                if (static_cast<std::int32_t>(now - next->due) >= static_cast<std::int32_t>(next->period)) {
                    next->due = now;
                }
            } else {
                timers_.erase(next->id);
            }
        }
        firedOnFrames_.fetch_add(1);
        next->callback(next->id);
    }
}

} // namespace sfcop::hle::kernel32rt
