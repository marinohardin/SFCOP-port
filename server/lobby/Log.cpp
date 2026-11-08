#include "Log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace sfcop::lobby::log {

namespace {

Sink& sink() {
    static Sink s;
    return s;
}

bool debugWanted() {
    static const bool wanted = [] {
        const char* v = std::getenv("SFCOP_LOG_LEVEL");
        return v && std::strcmp(v, "debug") == 0;
    }();
    return wanted;
}

const char* levelName(LogLevel l) {
    switch (l) {
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warn: return "warn";
    case LogLevel::Error: return "error";
    }
    return "?";
}

void writeToStdout(LogLevel level, const std::string& line) {
    if (level == LogLevel::Debug && !debugWanted()) return;
    std::time_t t = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&t, &utc);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", &utc);
    std::fprintf(stdout, "%s %s %s\n", stamp, levelName(level), line.c_str());
    std::fflush(stdout);
}

} // namespace

void setSink(Sink s) { sink() = std::move(s); }
void resetSink() { sink() = nullptr; }

void write(LogLevel level, const std::string& tag, const std::string& message) {
    std::string line = tag + ": " + message;
    if (sink()) {
        sink()(level, line);
    } else {
        writeToStdout(level, line);
    }
}

} // namespace sfcop::lobby::log
