// Time: GetTickCount, the performance counter, the wall clock as SYSTEMTIME and FILETIME, the
// conversions between them, and the local time zone's bias, on Clock. WINMM's timeGetTime is
// GetTickCount's value.
//
// The time zone is reported as its current offset only: GetTimeZoneInformation answers
// TIME_ZONE_ID_UNKNOWN with that bias and no daylight-saving rule, so a FILETIME converts to local
// time with today's offset, whatever its date. The game only stamps files and logs with it.
//
// Threading: any thread.
#pragma once

#include "platform/Clock/Clock.h"

#include <cstdint>
#include <optional>

namespace sfcop::hle::kernel32rt {

// SYSTEMTIME's eight 16-bit fields, in its order.
struct SystemTime {
    std::uint16_t year = 0;
    std::uint16_t month = 0;     // 1 to 12
    std::uint16_t dayOfWeek = 0; // 0 = Sunday
    std::uint16_t day = 0;
    std::uint16_t hour = 0;
    std::uint16_t minute = 0;
    std::uint16_t second = 0;
    std::uint16_t milliseconds = 0;
};

// FILETIME units per second (100 ns each), counted from 1601-01-01 00:00 UTC.
constexpr std::uint64_t kFileTimePerSecond = 10'000'000u;

// SystemTimeToFileTime: std::nullopt for a field out of range (the year 1601 to 30827, a day past
// the month's end, ...); dayOfWeek is ignored, as Windows ignores it.
std::optional<std::uint64_t> fileTimeFromSystemTime(const SystemTime &time);
// FileTimeToSystemTime: std::nullopt for a value with the top bit set.
std::optional<SystemTime> systemTimeFromFileTime(std::uint64_t fileTime);

class Time {
public:
    explicit Time(platform::Clock &clock);

    // GetTickCount and timeGetTime: milliseconds since the clock started, wrapping at 2^32.
    std::uint32_t TickCount();
    // QueryPerformanceCounter and QueryPerformanceFrequency; 0 if the clock has none.
    std::uint64_t Counter();
    std::uint64_t CounterFrequency();
    // GetSystemTime (UTC) and GetLocalTime.
    SystemTime Now(platform::TimeZone zone);
    // GetSystemTimeAsFileTime.
    std::uint64_t FileTimeNow();
    // The bias Windows reports: minutes to add to local time to get UTC (UTC+2 is -120).
    std::int32_t BiasMinutes();
    // FileTimeToLocalFileTime and LocalFileTimeToFileTime, with today's bias.
    std::uint64_t LocalFromUtc(std::uint64_t fileTime);
    std::uint64_t UtcFromLocal(std::uint64_t fileTime);

private:
    platform::Clock &clock_;
};

} // namespace sfcop::hle::kernel32rt
