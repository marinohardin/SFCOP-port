#include "hle/kernel32rt/Timers.h"

#include "platform/Clock/ClockMock.h"
#include "platform/Threads/ThreadsMock.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace sfcop::hle::kernel32rt {
namespace {

using platform::ClockMock;
using platform::ThreadsMock;
using platform::WaitOutcome;
using platform::WaitResult;

// The mock starts no thread: each test runs the timer's body itself, on its own thread.

TEST_CASE("a one-shot timer waits for its delay, then runs the callback once") {
    ThreadsMock threads;
    ClockMock clock;
    clock.ticksResult = std::uint32_t{1000};
    threads.waitResult = WaitResult{WaitOutcome::kTimedOut, 0};
    Timers timers(threads, clock);
    std::vector<std::uint32_t> fired;
    const std::uint32_t id = timers.Start(100, false, [&](std::uint32_t which) {
        fired.push_back(which);
    });
    CHECK(id == 1);
    threads.log.Clear();
    CHECK(threads.lastBody() == 0);
    CHECK(fired == std::vector<std::uint32_t>{1});
    // The fired one-shot gives its thread and stop event back.
    CHECK(threads.log.Calls() == std::vector<std::string>{"CurrentThreadId()", "Wait(1, 0, 100)",
                                                          "Close(1)", "Close(2)"});
}

TEST_CASE("a fired one-shot is gone from the table, so a second kill is false") {
    ThreadsMock threads;
    ClockMock clock;
    threads.waitResult = WaitResult{WaitOutcome::kTimedOut, 0};
    Timers timers(threads, clock);
    const std::uint32_t id = timers.Start(5, false, [](std::uint32_t) {});
    threads.lastBody();
    CHECK_FALSE(timers.Kill(id));
}

TEST_CASE("a timer is in the table before its thread starts, so its callback can kill it") {
    ThreadsMock threads;
    ClockMock clock;
    threads.waitResult = WaitResult{WaitOutcome::kTimedOut, 0};
    Timers timers(threads, clock);
    std::uint32_t id = 0;
    std::vector<std::string> seen;
    id = timers.Start(1, false, [&](std::uint32_t) { seen.push_back(timers.Kill(id) ? "killed" : "missed"); });
    threads.lastBody();
    CHECK(seen == std::vector<std::string>{"killed"});
}

TEST_CASE("a periodic timer stalled for ten periods fires at most twice on resume") {
    ThreadsMock threads;
    ClockMock clock;
    clock.ticksResult = std::uint32_t{0};
    threads.waitResult = WaitResult{WaitOutcome::kTimedOut, 0};
    Timers timers(threads, clock);
    int calls = 0;
    timers.Start(100, true, [&](std::uint32_t) {
        ++calls;
        if (calls == 1) {
            // The process was stalled: the clock is ten periods past the due time.
            clock.ticksResult = std::uint32_t{1100};
        }
        if (calls == 2) {
            threads.waitResult = WaitResult{WaitOutcome::kSignaled, 0}; // then stop
        }
    });
    threads.log.Clear();
    threads.lastBody();
    CHECK(calls == 2);
    // The first wait is for the delay; the catch-up tick waits 0; the cadence resumes at 100.
    CHECK(threads.log.Calls() ==
          std::vector<std::string>{"CurrentThreadId()", "Wait(1, 0, 100)", "Wait(1, 0, 0)",
                                   "Wait(1, 0, 100)"});
}

TEST_CASE("a periodic timer takes each due time from the last, so a late callback does not drift") {
    ThreadsMock threads;
    ClockMock clock;
    clock.ticksResult = std::uint32_t{0};
    threads.waitResult = WaitResult{WaitOutcome::kTimedOut, 0};
    Timers timers(threads, clock);
    int calls = 0;
    timers.Start(100, true, [&](std::uint32_t) {
        ++calls;
        // Each callback starts 130 ms after the previous due time.
        clock.ticksResult = std::uint32_t{static_cast<std::uint32_t>(calls * 100 + 30)};
        if (calls == 3) {
            threads.waitResult = WaitResult{WaitOutcome::kSignaled, 0}; // the stop event
        }
    });
    threads.log.Clear();
    threads.lastBody();
    CHECK(calls == 3);
    CHECK(threads.log.Calls() ==
          std::vector<std::string>{"CurrentThreadId()", "Wait(1, 0, 100)", "Wait(1, 0, 70)",
                                   "Wait(1, 0, 70)", "Wait(1, 0, 70)"});
}

TEST_CASE("a timer whose due time has passed fires without waiting") {
    ThreadsMock threads;
    ClockMock clock;
    clock.ticksResult = std::uint32_t{0};
    threads.waitResult = WaitResult{WaitOutcome::kTimedOut, 0};
    Timers timers(threads, clock);
    timers.Start(10, false, [](std::uint32_t) {});
    clock.ticksResult = std::uint32_t{50};
    threads.log.Clear();
    threads.lastBody();
    CHECK(threads.log.Calls()[1] == "Wait(1, 0, 0)"); // after CurrentThreadId()
}

TEST_CASE("timeKillEvent stops the timer and waits for its thread") {
    ThreadsMock threads;
    ClockMock clock;
    Timers timers(threads, clock);
    const std::uint32_t id = timers.Start(100, true, [](std::uint32_t) {});
    threads.currentThreadIdResult = std::uint32_t{5}; // the timer thread's id
    threads.waitResult = WaitResult{WaitOutcome::kSignaled, 0};
    threads.lastBody();
    threads.currentThreadIdResult = std::uint32_t{6}; // the caller is another thread
    threads.log.Clear();
    CHECK(timers.Kill(id));
    CHECK(threads.log.Calls() == std::vector<std::string>{"SetEvent(2)", "CurrentThreadId()",
                                                          "Wait(1, 0, \"forever\")", "Close(1)",
                                                          "Close(2)"});
    CHECK_FALSE(timers.Kill(id));
}

TEST_CASE("timeKillEvent from the timer's own callback does not wait for itself") {
    ThreadsMock threads;
    ClockMock clock;
    threads.waitResult = WaitResult{WaitOutcome::kTimedOut, 0};
    Timers timers(threads, clock);
    std::uint32_t id = 0;
    bool killed = false;
    id = timers.Start(1, false, [&](std::uint32_t) { killed = timers.Kill(id); });
    threads.log.Clear();
    threads.lastBody();
    CHECK(killed);
    CHECK(threads.log.Count("Wait") == 1); // only the timer's own wait for its due time
}

TEST_CASE("timeSetEvent answers 0 when no thread can be started") {
    ThreadsMock threads;
    ClockMock clock;
    threads.startThreadResult = platform::Error::kOutOfResources;
    Timers timers(threads, clock);
    CHECK(timers.Start(10, true, [](std::uint32_t) {}) == 0);
    CHECK(threads.log.Count("Close") == 1); // the stop event
}

} // namespace
} // namespace sfcop::hle::kernel32rt
