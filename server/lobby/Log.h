// The service's one logging facility (CONVENTIONS §4.2, DESIGN §9): a component tag, a level and
// a sentence. Nothing identifying is logged above Debug: callers pass ids the service chose
// (session ids), counts and path patterns, never an address, a name, a secret or a ticket.
// The one exception LOBBY_API.md names: a rate-limit or block event carries the source address.
#pragma once

#include <functional>
#include <string>

namespace sfcop::lobby {

enum class LogLevel { Debug, Info, Warn, Error };

namespace log {

using Sink = std::function<void(LogLevel, const std::string& line)>;

// The default sink writes "<UTC time> <level> <line>" to stdout, one line each, for the service
// manager to collect; Debug lines are dropped unless the environment asks (SFCOP_LOG_LEVEL=debug).
// Tests install their own sink.
void setSink(Sink sink);
void resetSink();

void write(LogLevel level, const std::string& tag, const std::string& message);

inline void debug(const std::string& tag, const std::string& message) { write(LogLevel::Debug, tag, message); }
inline void info(const std::string& tag, const std::string& message) { write(LogLevel::Info, tag, message); }
inline void warn(const std::string& tag, const std::string& message) { write(LogLevel::Warn, tag, message); }
inline void error(const std::string& tag, const std::string& message) { write(LogLevel::Error, tag, message); }

} // namespace log
} // namespace sfcop::lobby
