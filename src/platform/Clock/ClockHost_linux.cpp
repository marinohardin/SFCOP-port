// Clock's calendar conversion on Linux: the reentrant POSIX calls.

#include "platform/Clock/Clock.h"

#include <ctime>

namespace sfcop::platform::clockhost {

bool civilFromUnix(std::time_t seconds, TimeZone zone, std::tm &out) {
    return (zone == TimeZone::kUtc ? gmtime_r(&seconds, &out) : localtime_r(&seconds, &out)) !=
           nullptr;
}

} // namespace sfcop::platform::clockhost
