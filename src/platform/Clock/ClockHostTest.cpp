// The host Clock backend through the interface. The values come from the machine's clock, so the
// checks are the contract's invariants (ranges, order, agreement between methods) rather than
// exact times; the one test that measures a sleep is skipped under SFCOP_SKIP_TIMING_TESTS=1.

#include "platform/Clock/Clock.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <cstring>

#if !defined(_WIN32)
#include <sys/time.h>
#endif

namespace sfcop::platform {
namespace {

bool timingTestsSkipped() {
    const char *skip = std::getenv("SFCOP_SKIP_TIMING_TESTS");
    return skip != nullptr && std::strcmp(skip, "1") == 0;
}

// Days from 1970-01-01 to a proleptic Gregorian date (Howard Hinnant's algorithm), so that a
// CivilTime in UTC can be compared with UnixTime() without the code under test.
std::int64_t daysFromCivil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

} // namespace

TEST_CASE("the host counter runs at 10 MHz and the ticks and counter never go backwards") {
    auto clock = makeClock();
    REQUIRE(clock->CounterFrequency().Ok());
    CHECK(clock->CounterFrequency().Value() == 10000000u);
    const std::uint32_t t0 = clock->Ticks().Value();
    const std::uint64_t c0 = clock->Counter().Value();
    const std::uint64_t c1 = clock->Counter().Value();
    const std::uint32_t t1 = clock->Ticks().Value();
    CHECK(c1 >= c0);
    CHECK(t1 >= t0);
    // A new clock starts near zero: its origin is its creation.
    CHECK(t0 < 60000u);
}

TEST_CASE("the host wall clock gives calendar fields in range in both zones") {
    auto clock = makeClock();
    for (TimeZone zone : {TimeZone::kUtc, TimeZone::kLocal}) {
        Result<CivilTime> now = clock->Now(zone);
        REQUIRE(now.Ok());
        const CivilTime &t = now.Value();
        CHECK(t.year >= 2024);
        CHECK(t.month >= 1);
        CHECK(t.month <= 12);
        CHECK(t.day >= 1);
        CHECK(t.day <= 31);
        CHECK(t.weekday <= 6);
        CHECK(t.hour <= 23);
        CHECK(t.minute <= 59);
        CHECK(t.second <= 60);
        CHECK(t.millisecond <= 999);
    }
}

TEST_CASE("the host UTC calendar time and Unix time agree") {
    auto clock = makeClock();
    const std::int64_t before = clock->UnixTime().Value();
    const CivilTime utc = clock->Now(TimeZone::kUtc).Value();
    const std::int64_t after = clock->UnixTime().Value();
    const std::int64_t fromCivil =
        daysFromCivil(utc.year, utc.month, utc.day) * 86400 + utc.hour * 3600 + utc.minute * 60 +
        utc.second;
    CHECK(fromCivil >= before);
    CHECK(fromCivil <= after);
    // 2024-01-01 00:00:00 UTC: the clock is not at its epoch.
    CHECK(before > 1704067200);
    // 1970-01-04 was a Sunday, so the weekday follows from the day count.
    CHECK(utc.weekday == static_cast<std::uint8_t>((daysFromCivil(utc.year, utc.month, utc.day) + 4) % 7));
}

#if !defined(_WIN32)
// Two readings of the same clock, taken a moment apart, must agree: the expected value is the
// gettimeofday() read, not a wall time written by hand, because no fixed time can be expected.
TEST_CASE("the host wall clock reads gettimeofday, to the second") {
    auto clock = makeClock();
    timeval tv{};
    gettimeofday(&tv, nullptr);
    const std::int64_t unix = clock->UnixTime().Value();
    CHECK(unix >= static_cast<std::int64_t>(tv.tv_sec) - 1);
    CHECK(unix <= static_cast<std::int64_t>(tv.tv_sec) + 1);
    const CivilTime utc = clock->Now(TimeZone::kUtc).Value();
    const std::int64_t fromCivil =
        daysFromCivil(utc.year, utc.month, utc.day) * 86400 + utc.hour * 3600 + utc.minute * 60 +
        utc.second;
    CHECK(fromCivil >= static_cast<std::int64_t>(tv.tv_sec) - 1);
    CHECK(fromCivil <= static_cast<std::int64_t>(tv.tv_sec) + 1);
}
#endif

TEST_CASE("a zero sleep returns at once") {
    auto clock = makeClock();
    CHECK(clock->Sleep(0).Ok());
}

TEST_CASE("a host sleep lasts at least as long as asked" *
          doctest::skip(timingTestsSkipped())) {
    auto clock = makeClock();
    const std::uint64_t frequency = clock->CounterFrequency().Value();
    const std::uint64_t start = clock->Counter().Value();
    REQUIRE(clock->Sleep(20).Ok());
    const std::uint64_t elapsed = clock->Counter().Value() - start;
    CHECK(elapsed * 1000 >= 20 * frequency);
}

} // namespace sfcop::platform
