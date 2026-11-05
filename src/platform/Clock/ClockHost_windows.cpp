// Clock's calendar conversion on Windows: the C runtime's secure calls, which return 0 on success.

#include "platform/Clock/Clock.h"

#include <cstdint>
#include <ctime>

#include <windows.h>

namespace sfcop::platform::clockhost {

bool civilFromUnix(std::time_t seconds, TimeZone zone, std::tm &out) {
    return (zone == TimeZone::kUtc ? gmtime_s(&out, &seconds) : localtime_s(&out, &seconds)) == 0;
}

void wallNow(std::int64_t &seconds, std::int32_t &millis) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    // 100 ns units since 1601-01-01; 11644473600 s separate that from the Unix epoch.
    const std::uint64_t units =
        (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    seconds = static_cast<std::int64_t>(units / 10000000ull) - 11644473600ll;
    millis = static_cast<std::int32_t>(units / 10000ull % 1000ull);
}

} // namespace sfcop::platform::clockhost
