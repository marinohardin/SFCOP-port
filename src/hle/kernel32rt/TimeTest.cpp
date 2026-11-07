#include "hle/kernel32rt/Time.h"

#include "platform/Clock/ClockMock.h"
#include "platform/Clock/FixedStepClock.h"

#include <doctest/doctest.h>

#include <memory>

namespace sfcop::hle::kernel32rt {
namespace {

using platform::CivilTime;
using platform::ClockMock;
using platform::TimeZone;

// A clock whose local time is a fixed number of hours ahead of UTC.
class ZoneClock final : public platform::Clock {
public:
    CivilTime utc;
    CivilTime local;
    platform::Result<std::uint32_t> Ticks() override { return std::uint32_t{0}; }
    platform::Result<std::uint64_t> Counter() override { return std::uint64_t{0}; }
    platform::Result<std::uint64_t> CounterFrequency() override { return std::uint64_t{1}; }
    platform::Result<CivilTime> Now(TimeZone zone) override {
        return zone == TimeZone::kUtc ? utc : local;
    }
    platform::Result<std::int64_t> UnixTime() override { return std::int64_t{0}; }
    platform::Status Sleep(std::uint32_t) override { return {}; }
};

TEST_CASE("SystemTimeToFileTime counts 100 ns units from 1601") {
    CHECK(fileTimeFromSystemTime(SystemTime{1601, 1, 1, 1, 0, 0, 0, 0}) == 0u);
    CHECK(fileTimeFromSystemTime(SystemTime{1970, 1, 0, 1, 0, 0, 0, 0}) == 116444736000000000u);
    CHECK(fileTimeFromSystemTime(SystemTime{2003, 10, 0, 17, 12, 34, 56, 789}) ==
          127108676967890000u);
    CHECK(fileTimeFromSystemTime(SystemTime{2000, 2, 0, 29, 23, 59, 59, 999}) ==
          125963423999990000u);
}

TEST_CASE("SystemTimeToFileTime refuses a date that does not exist") {
    CHECK_FALSE(fileTimeFromSystemTime(SystemTime{1900, 2, 0, 29, 0, 0, 0, 0}));
    CHECK_FALSE(fileTimeFromSystemTime(SystemTime{2003, 4, 0, 31, 0, 0, 0, 0}));
    CHECK_FALSE(fileTimeFromSystemTime(SystemTime{1600, 12, 0, 31, 0, 0, 0, 0}));
    CHECK_FALSE(fileTimeFromSystemTime(SystemTime{2003, 13, 0, 1, 0, 0, 0, 0}));
    CHECK_FALSE(fileTimeFromSystemTime(SystemTime{2003, 1, 0, 1, 24, 0, 0, 0}));
    CHECK_FALSE(fileTimeFromSystemTime(SystemTime{2003, 1, 0, 1, 0, 0, 0, 1000}));
}

TEST_CASE("FileTimeToSystemTime gives the date, the time and the day of the week") {
    const auto time = systemTimeFromFileTime(127108676967890000u);
    REQUIRE(time);
    CHECK(time->year == 2003);
    CHECK(time->month == 10);
    CHECK(time->day == 17);
    CHECK(time->dayOfWeek == 5); // a Friday
    CHECK(time->hour == 12);
    CHECK(time->minute == 34);
    CHECK(time->second == 56);
    CHECK(time->milliseconds == 789);
    const auto epoch = systemTimeFromFileTime(0);
    REQUIRE(epoch);
    CHECK(epoch->dayOfWeek == 1); // 1601-01-01, a Monday
    CHECK_FALSE(systemTimeFromFileTime(0x8000000000000000u));
}

TEST_CASE("GetTickCount and the performance counter read the clock") {
    ClockMock clock;
    clock.ticksResult = std::uint32_t{4000};
    clock.counterResult = std::uint64_t{123456};
    clock.counterFrequencyResult = std::uint64_t{10000000};
    Time time(clock);
    CHECK(time.TickCount() == 4000);
    CHECK(time.Counter() == 123456);
    CHECK(time.CounterFrequency() == 10000000);
    clock.ticksResult = platform::Error::kUnsupported;
    CHECK(time.TickCount() == 0);
}

TEST_CASE("GetSystemTimeAsFileTime is the UTC wall clock") {
    ClockMock clock;
    clock.nowResult = CivilTime{2026, 11, 4, 3, 15, 38, 0, 0};
    Time time(clock);
    CHECK(time.FileTimeNow() == 134382802800000000u);
    const SystemTime now = time.Now(TimeZone::kUtc);
    CHECK(now.dayOfWeek == 3);
    CHECK(now.hour == 15);
}

TEST_CASE("the bias is UTC minus local time, and local file times use it") {
    ZoneClock clock;
    clock.utc = CivilTime{2026, 11, 4, 3, 15, 38, 0, 0};
    clock.local = CivilTime{2026, 11, 4, 3, 17, 38, 0, 0};
    Time time(clock);
    CHECK(time.BiasMinutes() == -120);
    const std::uint64_t twoHours = 2u * 3600u * kFileTimePerSecond;
    CHECK(time.LocalFromUtc(134382802800000000u) == 134382802800000000u + twoHours);
    CHECK(time.UtcFromLocal(134382802800000000u + twoHours) == 134382802800000000u);
}

TEST_CASE("a bias west of Greenwich is positive") {
    ZoneClock clock;
    clock.utc = CivilTime{2026, 11, 4, 3, 2, 0, 0, 0};
    clock.local = CivilTime{2026, 11, 3, 2, 21, 0, 0, 0};
    Time time(clock);
    CHECK(time.BiasMinutes() == 300);
}

TEST_CASE("GetTickCount, timeGetTime and the performance counter answer from a fixed-step clock") {
    auto inner = std::make_unique<ClockMock>();
    ClockMock *mock = inner.get();
    platform::FixedStepClock clock(std::move(inner), 166'670); // 16.667 ms per frame
    clock.BindToCallingThread();
    Time time(clock);
    CHECK(time.TickCount() == 0u);
    CHECK(time.Counter() == 0u);
    CHECK(time.CounterFrequency() == 10'000'000u);
    clock.AdvanceFrame();
    clock.AdvanceFrame();
    // GetTickCount and timeGetTime are both TickCount(): two frames are 33.334 ms.
    CHECK(time.TickCount() == 33u);
    CHECK(time.Counter() == 333'340u);
    CHECK(clock.Sleep(10).Ok()); // Sleep(10) on the game's main thread
    CHECK(time.TickCount() == 43u);
    CHECK(time.Counter() == 433'340u);
    // The time of day stays the inner clock's.
    mock->nowResult = CivilTime{2026, 11, 4, 3, 15, 38, 0, 0};
    CHECK(time.FileTimeNow() == 134382802800000000u);
    CHECK(mock->log.Count("Ticks") == 0);
    CHECK(mock->log.Count("Counter") == 0);
}

} // namespace
} // namespace sfcop::hle::kernel32rt
