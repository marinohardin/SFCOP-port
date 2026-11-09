// The directory's one log facility: a level and a sentence, tagged by the sink. Nothing
// identifying is written above debug level (DESIGN §9): a client's address appears only in debug
// lines. The sink is replaceable so a test can read every line.
#pragma once

#include <functional>
#include <string>
#include <utility>

namespace sfcop::directory {

enum class LogLevel { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };

class Logger {
public:
    using Sink = std::function<void(LogLevel, const std::string&)>;

    Logger(LogLevel threshold, Sink sink) : threshold_(threshold), sink_(std::move(sink)) {}

    bool enabled(LogLevel level) const { return level >= threshold_; }
    void log(LogLevel level, const std::string& sentence) const {
        if (enabled(level) && sink_) sink_(level, sentence);
    }
    void debug(const std::string& s) const { log(LogLevel::kDebug, s); }
    void info(const std::string& s) const { log(LogLevel::kInfo, s); }
    void warn(const std::string& s) const { log(LogLevel::kWarn, s); }
    void error(const std::string& s) const { log(LogLevel::kError, s); }

private:
    LogLevel threshold_;
    Sink sink_;
};

inline const char* levelName(LogLevel level) {
    switch (level) {
        case LogLevel::kDebug: return "debug";
        case LogLevel::kInfo: return "info";
        case LogLevel::kWarn: return "warn";
        default: return "error";
    }
}

inline bool parseLogLevel(const std::string& text, LogLevel& out) {
    if (text == "debug") out = LogLevel::kDebug;
    else if (text == "info") out = LogLevel::kInfo;
    else if (text == "warn") out = LogLevel::kWarn;
    else if (text == "error") out = LogLevel::kError;
    else return false;
    return true;
}

}  // namespace sfcop::directory
