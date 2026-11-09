#include "platform/Clock/FixedStepClock.h"
#include "platform/Clock/ClockMock.h"

#include <doctest/doctest.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace sfcop::platform {
namespace {

// A fixed-step clock on a mock, with the mock kept for the test to program and inspect. The
// quantum is 1000/60 ms to the microsecond: 166,670 units of 100 ns.
struct Rig {
    ClockMock *inner = nullptr;
    std::unique_ptr<FixedStepClock> clock;

    explicit Rig(std::uint64_t quantum = 166'670) {
        auto mock = std::make_unique<ClockMock>();
        inner = mock.get();
        clock = std::make_unique<FixedStepClock>(std::move(mock), quantum);
        clock->BindToCallingThread();
    }
};

} // namespace

TEST_CASE("the fixed-step ticks start at 0 and grow by the quantum per frame") {
    Rig rig;
    CHECK(rig.clock->Ticks().Value() == 0u);
    CHECK(rig.clock->Ticks().Value() == 0u); // reading does not move them
    rig.clock->AdvanceFrame();
    CHECK(rig.clock->Ticks().Value() == 16u); // 16.667 ms
    rig.clock->AdvanceFrame();
    CHECK(rig.clock->Ticks().Value() == 33u); // 33.334 ms
    rig.clock->AdvanceFrame();
    CHECK(rig.clock->Ticks().Value() == 50u); // 50.001 ms
    CHECK(rig.clock->Frames() == 3u);
    CHECK(rig.inner->log.Count("Ticks") == 0); // the inner clock's ticks are never read
}

TEST_CASE("the fixed-step ticks wrap at 2^32 as the game's tick counters do") {
    // One frame of 2^32 ms plus 5 ms, in 100 ns units.
    Rig rig(42'949'672'960'000ull + 50'000ull);
    rig.clock->AdvanceFrame();
    CHECK(rig.clock->Ticks().Value() == 5u);
}

TEST_CASE("the fixed-step counter is the count in 100 ns units") {
    Rig rig;
    CHECK(rig.clock->Counter().Value() == 0u);
    rig.clock->AdvanceFrame();
    CHECK(rig.clock->Counter().Value() == 166'670u);
    rig.clock->AdvanceFrame();
    CHECK(rig.clock->Counter().Value() == 333'340u);
    CHECK(rig.inner->log.Count("Counter") == 0);
}

TEST_CASE("the fixed-step counter frequency is 10 MHz whatever the inner clock says") {
    Rig rig;
    rig.inner->counterFrequencyResult = std::uint64_t{1000};
    CHECK(rig.clock->CounterFrequency().Value() == 10'000'000u);
    CHECK(rig.inner->log.Count("CounterFrequency") == 0);
}

TEST_CASE("the fixed-step time of day starts at the pinned epoch, the same in both zones") {
    Rig rig;
    rig.inner->nowResult = CivilTime{2003, 10, 17, 5, 9, 30, 0, 0}; // the host's, never read
    rig.inner->unixTimeResult = std::int64_t{1066392000};
    for (const TimeZone zone : {TimeZone::kUtc, TimeZone::kLocal}) {
        const Result<CivilTime> now = rig.clock->Now(zone);
        REQUIRE(now.Ok());
        CHECK(now.Value().year == 2026);
        CHECK(now.Value().month == 1);
        CHECK(now.Value().day == 1);
        CHECK(now.Value().weekday == 4); // Thursday
        CHECK(now.Value().hour == 0);
        CHECK(now.Value().minute == 0);
        CHECK(now.Value().second == 0);
        CHECK(now.Value().millisecond == 0);
    }
    CHECK(rig.clock->UnixTime().Value() == 1'767'225'600);
    CHECK(rig.inner->log.Count("Now") == 0);
    CHECK(rig.inner->log.Count("UnixTime") == 0);
}

TEST_CASE("the fixed-step time of day moves with the count, never with the host") {
    Rig rig;
    for (int i = 0; i < 3; ++i) {
        rig.clock->AdvanceFrame(); // 50.001 ms
    }
    CHECK(rig.clock->Now(TimeZone::kUtc).Value().millisecond == 50);
    CHECK(rig.clock->UnixTime().Value() == 1'767'225'600);
    CHECK(rig.clock->Sleep(61'000).Ok()); // a minute and a second on the main thread
    const CivilTime later = rig.clock->Now(TimeZone::kLocal).Value();
    CHECK(later.minute == 1);
    CHECK(later.second == 1);
    CHECK(later.millisecond == 50);
    CHECK(rig.clock->UnixTime().Value() == 1'767'225'661);
}

TEST_CASE("reading the fixed-step time of day is not a read of the clock") {
    Rig rig;
    for (int i = 0; i < 20'000; ++i) {
        static_cast<void>(rig.clock->Now(TimeZone::kUtc));
        static_cast<void>(rig.clock->UnixTime());
    }
    CHECK(rig.clock->SpinSteps() == 0u);
    CHECK(rig.clock->Counter().Value() == 0u);
}

TEST_CASE("the pinned calendar converts Unix seconds to a UTC date") {
    const CivilTime epoch = civilTimeFromUnix(0, 0);
    CHECK(epoch.year == 1970);
    CHECK(epoch.month == 1);
    CHECK(epoch.day == 1);
    CHECK(epoch.weekday == 4); // Thursday
    const CivilTime leap = civilTimeFromUnix(951'782'400, 999); // 2000-02-29 00:00:00.999
    CHECK(leap.year == 2000);
    CHECK(leap.month == 2);
    CHECK(leap.day == 29);
    CHECK(leap.weekday == 2); // Tuesday
    CHECK(leap.millisecond == 999);
    const CivilTime noon = civilTimeFromUnix(1'066'392'000, 0); // 2003-10-17 12:00:00
    CHECK(noon.year == 2003);
    CHECK(noon.month == 10);
    CHECK(noon.day == 17);
    CHECK(noon.weekday == 5); // Friday
    CHECK(noon.hour == 12);
    const CivilTime before = civilTimeFromUnix(-1, 0); // 1969-12-31 23:59:59
    CHECK(before.year == 1969);
    CHECK(before.month == 12);
    CHECK(before.day == 31);
    CHECK(before.weekday == 3); // Wednesday
    CHECK(before.hour == 23);
    CHECK(before.minute == 59);
    CHECK(before.second == 59);
    const CivilTime pinned = civilTimeFromUnix(FixedStepClock::kEpochUnixSeconds, 0);
    CHECK(pinned.year == 2026);
    CHECK(pinned.month == 1);
    CHECK(pinned.day == 1);
}

TEST_CASE("a sleep on the frame thread grows the count by the milliseconds asked and sleeps") {
    Rig rig;
    CHECK(rig.clock->Sleep(5).Ok());
    CHECK(rig.clock->Ticks().Value() == 5u);
    CHECK(rig.clock->Counter().Value() == 50'000u);
    CHECK(rig.clock->Sleep(0).Ok()); // gives up the time slice, no time passes
    CHECK(rig.clock->Ticks().Value() == 5u);
    rig.clock->AdvanceFrame();
    CHECK(rig.clock->Ticks().Value() == 21u); // 5 + 16.667
    CHECK(rig.inner->log.Calls() == std::vector<std::string>{"Sleep(5)", "Sleep(0)"});
    CHECK(rig.clock->Frames() == 1u);
}

TEST_CASE("a sleep on another thread leaves the count alone and still returns") {
    Rig rig;
    std::thread other([&] {
        for (int i = 0; i < 3; ++i) {
            CHECK(rig.clock->Sleep(10).Ok());
        }
    });
    other.join(); // it returned: a thread that sleeps never waits for a frame
    CHECK(rig.clock->Ticks().Value() == 0u);
    CHECK(rig.inner->log.Count("Sleep") == 3);
}

TEST_CASE("a thread that waits for ticks to pass ends while the frame thread pumps frames") {
    Rig rig;
    std::atomic<bool> done{false};
    std::thread waiter([&] {
        const std::uint32_t start = rig.clock->Ticks().Value();
        while (rig.clock->Ticks().Value() - start < 100) {
            static_cast<void>(rig.clock->Sleep(1));
        }
        done = true;
    });
    int frames = 0;
    while (!done && frames < 1000) {
        rig.clock->AdvanceFrame();
        ++frames;
        std::this_thread::yield();
    }
    waiter.join();
    CHECK(done);
    CHECK(frames >= 6); // 100 ms is six frames of 16.667 ms
}

TEST_CASE("the frame thread spinning on the clock moves it a millisecond per 10,000 reads") {
    Rig rig;
    std::uint32_t ticks = 0;
    for (std::uint64_t i = 0; i < 9'999; ++i) {
        ticks = rig.clock->Ticks().Value();
    }
    CHECK(ticks == 0u);
    CHECK(rig.clock->SpinSteps() == 0u);
    CHECK(rig.clock->Counter().Value() == 10'000u); // the 10,000th read
    CHECK(rig.clock->SpinSteps() == 1u);
    rig.clock->AdvanceFrame(); // a frame starts the count of reads again
    for (std::uint64_t i = 0; i < 9'999; ++i) {
        ticks = rig.clock->Ticks().Value();
    }
    CHECK(ticks == 17u); // 1 + 16.667 ms
    CHECK(rig.clock->SpinSteps() == 1u);
}

TEST_CASE("reads on another thread never move the fixed-step clock") {
    Rig rig;
    std::thread other([&] {
        for (std::uint64_t i = 0; i < 10'001; ++i) {
            static_cast<void>(rig.clock->Ticks());
        }
    });
    other.join();
    CHECK(rig.clock->Counter().Value() == 0u);
    CHECK(rig.clock->SpinSteps() == 0u);
}

TEST_CASE("an unbound fixed-step clock does not grow on sleep") {
    auto mock = std::make_unique<ClockMock>();
    FixedStepClock clock(std::move(mock), 166'670);
    CHECK(clock.Sleep(7).Ok());
    CHECK(clock.Ticks().Value() == 0u);
    clock.AdvanceFrame();
    CHECK(clock.Ticks().Value() == 16u);
}

TEST_CASE("the default fixed step is 1000/60 ms to the microsecond") {
    CHECK(kDefaultFixedStepMicroseconds == 16'667u);
    CHECK(fixedStepQuantumFromMicroseconds(16'667) == 166'670u);
    CHECK(fixedStepQuantumFromMicroseconds(1) == 10u);
}

} // namespace sfcop::platform
