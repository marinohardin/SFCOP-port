#include "hle/kernel32rt/Runtime.h"

namespace sfcop::hle::kernel32rt {

Runtime &Runtime::Instance() {
    static Runtime runtime;
    return runtime;
}

void Runtime::Attach(platform::Threads &threads, platform::Clock &clock) {
    timers_.reset(); // stops the timers of an earlier attachment first
    threads_ = &threads;
    clock_ = &clock;
    sync_ = std::make_unique<Sync>(threads);
    sections_ = std::make_unique<CriticalSections>();
    slots_ = std::make_unique<ThreadLocal>(threads);
    time_ = std::make_unique<Time>(clock);
    timers_ = std::make_unique<Timers>(threads, clock);
    filter_ = 0;
}

void Runtime::Detach() {
    timers_.reset();
    time_.reset();
    slots_.reset();
    sections_.reset();
    sync_.reset();
    clock_ = nullptr;
    threads_ = nullptr;
    filter_ = 0;
}

} // namespace sfcop::hle::kernel32rt
