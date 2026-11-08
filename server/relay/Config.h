// Command line, key and block list parsing. The ticket key comes from the environment
// (SFCOP_TICKET_KEY, 64 hexadecimal digits), which the host's environment file sets; it is never
// read from the repository and never printed.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>

#include "Log.h"
#include "Wire.h"

namespace sfcop::relay {

struct Options {
    std::uint16_t port = 47710;
    std::uint16_t healthPort = 47711;
    std::string bindAddress = "0.0.0.0";
    std::string healthAddress = "127.0.0.1";
    std::string blocklistPath;
    LogLevel logLevel = LogLevel::kInfo;
    bool help = false;
};

// Applies SFCOP_RELAY_PORT (the UDP port), SFCOP_HEALTH_LISTEN (host:port of the loopback HTTP
// listener, the name the host's monitor reads too) and SFCOP_LOG_LEVEL over the defaults; `getenv` is a seam for tests. False names a bad value in `error`.
bool applyEnvironment(Options& out, const char* (*getenv)(const char*), std::string& error);

// `--port N --health-port N --bind ADDR --health-bind ADDR --blocklist FILE --log-level L `.
bool parseArgs(int argc, const char* const* argv, Options& out, std::string& error);

// Exactly 64 hexadecimal digits (either case) into 32 bytes.
bool parseLogLevel(const std::string& text, LogLevel& out);

bool parseHexKey(const std::string& text, Bytes& out);

// One dotted-quad IPv4 address per line; blank lines and `#` comments are skipped. A bad line
// fails the whole file, so a typo is not read as "block nobody".
bool parseBlocklist(const std::string& text, std::unordered_set<std::uint32_t>& out,
                    std::string& error);

bool parseIPv4(const std::string& text, std::uint32_t& out);

}  // namespace sfcop::relay
