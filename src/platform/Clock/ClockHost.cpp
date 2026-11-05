// Clock's host backend (makeClock): std::chrono's steady clock for the ticks and the counter, the
// platform file's wall-time read (wallNow) for the time of day, and the calendar conversion of one file per platform
// (ClockHost_<platform>.cpp), which differ only in how the C library is asked for local time.
//
// The counter runs at 10 MHz, the frequency 64-bit Windows reports for QueryPerformanceFrequency;
// a game that multiplies the counter before dividing by the frequency then sees the magnitudes it
// was written for.

#include "platform/Clock/Clock.h"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <thread>

namespace sfcop::platform {

namespace clockhost {
// The platform file's half: `seconds` (Unix time) as a calendar time in `zone`; false if the
// C library cannot convert it.
bool civilFromUnix(std::time_t seconds, TimeZone zone, std::tm &out);
// The platform file's other half: the wall time as Unix seconds and the milliseconds past them.
// It is read from gettimeofday (GetSystemTimeAsFileTime on Windows), not from std::chrono's
// system clock, which libc++ also uses for the deadlines of timed waits: a tool that shifts the
// time of day (tools/fakeclock.c) must not shift those.
void wallNow(std::int64_t &seconds, std::int32_t &millis);
} // namespace clockhost

namespace {

constexpr std::uint64_t kCounterFrequency = 10'000'000;

class ClockHost final : public Clock {
public:
    ClockHost() : origin_(std::chrono::steady_clock::now()) {}

    Result<std::uint32_t> Ticks() override {
        // Truncated to 32 bits: the counter wraps as the game's tick counters do.
        return static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Elapsed()).count());
    }

    Result<std::uint64_t> Counter() override {
        using Units = std::chrono::duration<std::int64_t, std::ratio<1, kCounterFrequency>>;
        return static_cast<std::uint64_t>(std::chrono::duration_cast<Units>(Elapsed()).count());
    }

    Result<std::uint64_t> CounterFrequency() override { return kCounterFrequency; }

    Result<CivilTime> Now(TimeZone zone) override {
        std::int64_t seconds = 0;
        std::int32_t millis = 0;
        clockhost::wallNow(seconds, millis);
        std::tm parts{};
        if (!clockhost::civilFromUnix(static_cast<std::time_t>(seconds), zone, parts)) {
            return Error::kIoError;
        }
        CivilTime civil;
        civil.year = parts.tm_year + 1900;
        civil.month = static_cast<std::uint8_t>(parts.tm_mon + 1);
        civil.day = static_cast<std::uint8_t>(parts.tm_mday);
        civil.weekday = static_cast<std::uint8_t>(parts.tm_wday);
        civil.hour = static_cast<std::uint8_t>(parts.tm_hour);
        civil.minute = static_cast<std::uint8_t>(parts.tm_min);
        civil.second = static_cast<std::uint8_t>(parts.tm_sec);
        civil.millisecond = static_cast<std::uint16_t>(millis);
        return civil;
    }

    Result<std::int64_t> UnixTime() override {
        std::int64_t seconds = 0;
        std::int32_t millis = 0;
        clockhost::wallNow(seconds, millis);
        return seconds;
    }

    Status Sleep(std::uint32_t milliseconds) override {
        if (milliseconds == 0) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
        }
        return {};
    }

private:
    std::chrono::steady_clock::duration Elapsed() const {
        return std::chrono::steady_clock::now() - origin_;
    }

    const std::chrono::steady_clock::time_point origin_;
};

} // namespace

std::unique_ptr<Clock> makeClock() { return std::make_unique<ClockHost>(); }

} // namespace sfcop::platform
