// Runtime: the one kernel32rt the process has, holding the parts its rows share (the kernel
// objects, critical sections, thread-local slots, time, timers, built-in modules, the top-level
// exception filter). The guest host attaches Threads and Clock to it before the game runs
// (src/app/guest/); until then every part is absent and Attached() is false.
//
// Threading: Attach() once, before any guest code runs; the parts are safe from any thread.
#pragma once

#include "hle/kernel32rt/CriticalSections.h"
#include "hle/kernel32rt/Modules.h"
#include "hle/kernel32rt/Sync.h"
#include "hle/kernel32rt/ThreadLocal.h"
#include "hle/kernel32rt/Time.h"
#include "hle/kernel32rt/Timers.h"
#include "platform/Clock/Clock.h"
#include "platform/Threads/Threads.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace sfcop::hle::kernel32rt {

// The type this module's guest handles have in win32hle's handle table (src/hle/README.md,
// "Handle types").
constexpr int kHandleType = 26;

class Runtime {
public:
    static Runtime &Instance();

    // Builds the parts on `threads` and `clock`, which outlive the runtime's use. Replaces an
    // earlier attachment (tests only: the game's objects are gone with it).
    void Attach(platform::Threads &threads, platform::Clock &clock);
    // Stops the timers and drops the parts, before the components go. Tests call it; the guest
    // host does not, because the game ends as ExitProcess does, with its other threads still
    // running (src/app/guest/GuestHost.cpp, onExit).
    void Detach();
    bool Attached() const { return sync_ != nullptr; }

    platform::Threads &HostThreads() { return *threads_; }
    platform::Clock &HostClock() { return *clock_; }
    Sync &Objects() { return *sync_; }
    CriticalSections &Sections() { return *sections_; }
    ThreadLocal &Slots() { return *slots_; }
    Time &Timing() { return *time_; }
    Timers &TimerQueue() { return *timers_; }
    Modules &BuiltIns() { return modules_; }

    // The Threads object behind a guest HANDLE this module gave out (an event the game hands
    // DirectInput, say), or std::nullopt. The guest shims install the lookup
    // (Kernel32rtGuest.cpp); without them every handle names nothing.
    using HandleLookup = std::function<std::optional<platform::Waitable>(std::uint32_t)>;
    void SetHandleLookup(HandleLookup lookup) { lookup_ = std::move(lookup); }
    std::optional<platform::Waitable> WaitableOf(std::uint32_t handle) const {
        return lookup_ ? lookup_(handle) : std::nullopt;
    }

    // SetUnhandledExceptionFilter: stores the guest's filter and returns the previous one.
    std::uint32_t SetUnhandledFilter(std::uint32_t filter) { return filter_.exchange(filter); }
    std::uint32_t UnhandledFilter() const { return filter_.load(); }

private:
    platform::Threads *threads_ = nullptr;
    platform::Clock *clock_ = nullptr;
    std::unique_ptr<Sync> sync_;
    std::unique_ptr<CriticalSections> sections_;
    std::unique_ptr<ThreadLocal> slots_;
    std::unique_ptr<Time> time_;
    std::unique_ptr<Timers> timers_;
    Modules modules_;
    std::atomic<std::uint32_t> filter_{0};
    HandleLookup lookup_;
};

} // namespace sfcop::hle::kernel32rt
