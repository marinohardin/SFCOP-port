// Clock's calendar conversion and wall-time read on macOS: the reentrant POSIX calls.

#include "platform/Clock/Clock.h"

#include <cstdint>
#include <ctime>

#include <sys/time.h>

namespace sfcop::platform::clockhost {

bool civilFromUnix(std::time_t seconds, TimeZone zone, std::tm &out) {
    return (zone == TimeZone::kUtc ? gmtime_r(&seconds, &out) : localtime_r(&seconds, &out)) !=
           nullptr;
}

void wallNow(std::int64_t &seconds, std::int32_t &millis) {
    timeval tv{};
    gettimeofday(&tv, nullptr);
    seconds = static_cast<std::int64_t>(tv.tv_sec);
    millis = static_cast<std::int32_t>(tv.tv_usec / 1000);
}

} // namespace sfcop::platform::clockhost
