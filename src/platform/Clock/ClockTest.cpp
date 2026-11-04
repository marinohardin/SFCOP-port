#include "platform/Clock/Clock.h"
#include "platform/Clock/ClockMock.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace sfcop::platform {
namespace {

// A caller written against the interface only: the milliseconds since `start`, across a wrap of
// the 32-bit tick counter, or 0 if the clock fails.
std::uint32_t elapsedSince(Clock &clock, std::uint32_t start) {
    Result<std::uint32_t> now = clock.Ticks();
    return now.Ok() ? now.Value() - start : 0;
}

} // namespace

TEST_CASE("a caller measures elapsed ticks across the 32-bit wrap through the clock interface") {
    ClockMock mock;
    mock.ticksResult = std::uint32_t{0x00000010};
    CHECK(elapsedSince(mock, 0xFFFFFFF0u) == 0x20);
    mock.ticksResult = Error::kUnsupported;
    CHECK(elapsedSince(mock, 5) == 0);
    CHECK(mock.log.Calls() == std::vector<std::string>{"Ticks()", "Ticks()"});
}

TEST_CASE("the clock mock records its arguments and returns the programmed wall time") {
    ClockMock mock;
    CivilTime programmed;
    programmed.year = 2003;
    programmed.month = 10;
    programmed.day = 17;
    programmed.weekday = 5;
    programmed.hour = 12;
    mock.nowResult = programmed;
    Clock &clock = mock;
    Result<CivilTime> now = clock.Now(TimeZone::kLocal);
    REQUIRE(now.Ok());
    CHECK(now.Value().year == 2003);
    CHECK(now.Value().weekday == 5);
    CHECK(clock.Sleep(15).Ok());
    CHECK(clock.CounterFrequency().Value() == 1000);
    CHECK(mock.log.Calls() ==
          std::vector<std::string>{"Now(1)", "Sleep(15)", "CounterFrequency()"});
}

TEST_CASE("every method of the clock stub reports unsupported") {
    std::unique_ptr<Clock> clock = makeClockStub();
    CHECK(clock->Ticks().Code() == Error::kUnsupported);
    CHECK(clock->Counter().Code() == Error::kUnsupported);
    CHECK(clock->CounterFrequency().Code() == Error::kUnsupported);
    CHECK(clock->Now(TimeZone::kUtc).Code() == Error::kUnsupported);
    CHECK(clock->UnixTime().Code() == Error::kUnsupported);
    CHECK(clock->Sleep(0).Code() == Error::kUnsupported);
}

} // namespace sfcop::platform
