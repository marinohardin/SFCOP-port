#include "platform/Core/Log.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

FILE *g_important = nullptr;

} // namespace

namespace sfcop::platform {

void FormatLogLine(char *out, std::size_t size, const char *format, std::va_list args) {
    const int length = std::vsnprintf(out, size, format, args);
    if (length < 0) {
        std::snprintf(out, size, "(a log line could not be formatted: %s)\n", format);
    } else if (static_cast<std::size_t>(length) >= size && size >= 5) {
        std::memcpy(out + size - 5, "...\n", 5); // cut: the line still ends, with its NUL
    }
}

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
    sfcop::platform::FormatLogLine(text, sizeof text, format, args);
    va_end(args);
    sfcop::platform::RouteLog(severity, text, stderr, g_important);
}
