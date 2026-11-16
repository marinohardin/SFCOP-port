#include "platform/Core/Log.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace {

FILE *g_important = nullptr;

} // namespace

namespace sfcop::platform {

void RouteLog(int severity, const char *text, FILE *full, FILE *important) {
    std::fputs(text, full);
    if (severity >= SFCOP_LOG_IMPORTANT && important != nullptr) {
        std::fputs(text, important);
        std::fflush(important);
    }
}

} // namespace sfcop::platform

extern "C" void sfcop_log_init(void) {
    if (g_important != nullptr) {
        return;
    }
    if (const char *path = std::getenv("SFCOP_IMPORTANT_LOG"); path != nullptr && *path != '\0') {
        g_important = std::fopen(path, "ab");
    }
}

extern "C" void sfcop_logf(int severity, const char *format, ...) {
    char text[2048];
    std::va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof text, format, args);
    va_end(args);
    sfcop::platform::RouteLog(severity, text, stderr, g_important);
}
