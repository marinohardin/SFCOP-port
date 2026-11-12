// The module's parts on the real Threads and Clock backends, not the mocks: what the mocks cannot
// show, that the pieces work together on a real scheduler. Threads hand off through events and
// waits without a timeout, so no case depends on timing; the one that waits out a delay is
// skipped under SFCOP_SKIP_TIMING_TESTS=1.
#include "hle/kernel32rt/Sync.h"
#include "hle/kernel32rt/Timers.h"
#include "platform/Clock/Clock.h"
#include "platform/Threads/Threads.h"

#include <doctest/doctest.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace sfcop::hle::kernel32rt {
namespace {

bool timingTestsSkipped() {
    const char *skip = std::getenv("SFCOP_SKIP_TIMING_TESTS");
    return skip != nullptr && std::strcmp(skip, "1") == 0;
}

// Joins a Sync thread: waits for it and returns its exit code.
std::uint32_t join(Sync &sync, const Object &thread) {
    REQUIRE(sync.Wait({&thread}, false, kInfinite).value == kWaitObject0);
    std::uint32_t code = 0;
    REQUIRE(sync.GetExitCode(thread, code).value == 1);
    return code;
}

} // namespace

TEST_CASE("Sync waits on real events, mutexes and threads") {
    auto threads = platform::makeThreads();
    Sync sync(*threads);
    Object manual, autoReset;
    REQUIRE(sync.MakeEvent(true, true, "", manual).value == 1);
    REQUIRE(sync.MakeEvent(false, true, "", autoReset).value == 1);
    CHECK(sync.Wait({&manual}, false, 0).value == kWaitObject0);
    CHECK(sync.Wait({&manual}, false, 0).value == kWaitObject0); // stays signalled
    CHECK(sync.Wait({&autoReset}, false, 0).value == kWaitObject0);
    CHECK(sync.Wait({&autoReset}, false, 0).value == kWaitTimeout); // reset by the first wait
    CHECK(sync.Wait({&autoReset, &manual}, false, 0).value == kWaitObject0 + 1);
    CHECK(sync.ResetEvent(manual).value == 1);
    CHECK(sync.Wait({&manual}, false, 0).value == kWaitTimeout);

    Object mutex, thread;
    REQUIRE(sync.MakeMutex(false, "", mutex).value == 1);
    REQUIRE(sync.StartThread(
                [&] { return sync.Wait({&mutex}, false, kInfinite).value == kWaitObject0 ? 0u : 1u; },
                false, 0, thread).value == 1);
    CHECK(join(sync, thread) == 0u);
    // The thread ended owning the mutex: the next wait is told so, and owns it.
    CHECK(sync.Wait({&mutex}, false, 0).value == kWaitAbandoned0);
    CHECK(sync.ReleaseMutex(mutex).value == 1);
}

TEST_CASE("a timer killed while it waits never fires, and a second kill is false") {
    auto threads = platform::makeThreads();
    auto clock = platform::makeClock();
    Timers timers(*threads, *clock);
    std::atomic<int> fired{0};
    const std::uint32_t id = timers.Start(600000, true, [&](std::uint32_t) { fired++; });
    REQUIRE(id != 0);
    CHECK(timers.Kill(id)); // wakes the timer's wait and joins its thread
    CHECK_FALSE(timers.Kill(id));
    CHECK(fired.load() == 0);
}

TEST_CASE("a one-shot timer fires once on its own thread" * doctest::skip(timingTestsSkipped())) {
    auto threads = platform::makeThreads();
    auto clock = platform::makeClock();
    Sync sync(*threads);
    Timers timers(*threads, *clock);
    Object fired;
    REQUIRE(sync.MakeEvent(true, false, "", fired).value == 1);
    std::atomic<int> count{0};
    const std::uint32_t id = timers.Start(5, false, [&](std::uint32_t which) {
        count++;
        CHECK(which != 0);
        sync.SetEvent(fired);
    });
    REQUIRE(id != 0);
    CHECK(sync.Wait({&fired}, false, kInfinite).value == kWaitObject0);
    CHECK(count.load() == 1);
}

} // namespace sfcop::hle::kernel32rt
