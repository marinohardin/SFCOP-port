// Clock: the game's time sources (DESIGN §4.7, §6.2). Monotonic millisecond ticks (what the game
// reads through timeGetTime and GetTickCount), a high-resolution counter (QueryPerformanceCounter),
// the wall clock (GetLocalTime, GetSystemTime) and sleep. The kernel and winmm HLE modules are
// its callers; the oracle replaces it with a scripted clock so that a replay is deterministic.
//
// Threading: every method may be called from any thread at the same time.
#pragma once

#include "platform/Core/Result.h"

#include <cstdint>
#include <memory>

namespace sfcop::platform {

// A calendar date and time of day, in UTC or in the local time zone.
struct CivilTime {
    std::int32_t year = 0;        // four digits, e.g. 2003
    std::uint8_t month = 0;       // 1 to 12
    std::uint8_t day = 0;         // 1 to 31
    std::uint8_t weekday = 0;     // 0 = Sunday to 6 = Saturday
    std::uint8_t hour = 0;        // 0 to 23
    std::uint8_t minute = 0;      // 0 to 59
    std::uint8_t second = 0;      // 0 to 59 (60 only if the system reports a leap second)
    std::uint16_t millisecond = 0; // 0 to 999
};

enum class TimeZone { kUtc, kLocal };

class Clock {
public:
    virtual ~Clock() = default;

    // Milliseconds since an arbitrary origin fixed when the clock was created. Never goes
    // backwards and is not affected by changes to the wall clock; wraps to 0 after 2^32 - 1, as
    // the game's 32-bit tick counters do. Errors: kUnsupported.
    virtual Result<std::uint32_t> Ticks() = 0;

    // A high-resolution monotonic counter, in units of 1 / CounterFrequency() seconds, from the
    // same origin as Ticks(). Never goes backwards; does not wrap within the life of a process.
    // Errors: kUnsupported.
    virtual Result<std::uint64_t> Counter() = 0;

    // How many Counter() units make one second. Constant for the life of the clock, and at least
    // 1000. Errors: kUnsupported.
    virtual Result<std::uint64_t> CounterFrequency() = 0;

    // The current wall-clock date and time in `zone`. May jump when the user or the network
    // changes the system clock; never use it to measure intervals. Errors: kUnsupported;
    // kIoError if the system cannot report the time or the local time zone.
    virtual Result<CivilTime> Now(TimeZone zone) = 0;

    // Seconds since 1970-01-01 00:00:00 UTC, from the same source as Now(). Errors: kUnsupported;
    // kIoError as Now().
    virtual Result<std::int64_t> UnixTime() = 0;

    // Blocks the calling thread for at least `milliseconds`, as the scheduler allows. Zero gives
    // up the rest of the thread's time slice and returns. Errors: kUnsupported.
    virtual Status Sleep(std::uint32_t milliseconds) = 0;
};

// A clock whose every method fails with kUnsupported.
std::unique_ptr<Clock> makeClockStub();

} // namespace sfcop::platform
