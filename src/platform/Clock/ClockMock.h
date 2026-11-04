// A Clock for tests: records each call and returns the values the test programs.
#pragma once

#include "platform/Clock/Clock.h"
#include "platform/Core/MockLog.h"

namespace sfcop::platform {

class ClockMock final : public Clock {
public:
    MockLog log;

    Result<std::uint32_t> ticksResult = std::uint32_t{0};
    Result<std::uint64_t> counterResult = std::uint64_t{0};
    Result<std::uint64_t> counterFrequencyResult = std::uint64_t{1000};
    Result<CivilTime> nowResult = CivilTime{};
    Result<std::int64_t> unixTimeResult = std::int64_t{0};
    Status sleepResult;

    Result<std::uint32_t> Ticks() override {
        log.Record("Ticks");
        return ticksResult;
    }
    Result<std::uint64_t> Counter() override {
        log.Record("Counter");
        return counterResult;
    }
    Result<std::uint64_t> CounterFrequency() override {
        log.Record("CounterFrequency");
        return counterFrequencyResult;
    }
    Result<CivilTime> Now(TimeZone zone) override {
        log.Record("Now", zone);
        return nowResult;
    }
    Result<std::int64_t> UnixTime() override {
        log.Record("UnixTime");
        return unixTimeResult;
    }
    Status Sleep(std::uint32_t milliseconds) override {
        log.Record("Sleep", milliseconds);
        return sleepResult;
    }
};

} // namespace sfcop::platform
