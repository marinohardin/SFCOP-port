// Clock's calendar conversion on Windows: the C runtime's secure calls, which return 0 on success.

#include "platform/Clock/Clock.h"

#include <ctime>

namespace sfcop::platform::clockhost {

bool civilFromUnix(std::time_t seconds, TimeZone zone, std::tm &out) {
    return (zone == TimeZone::kUtc ? gmtime_s(&out, &seconds) : localtime_s(&out, &seconds)) == 0;
}

} // namespace sfcop::platform::clockhost
