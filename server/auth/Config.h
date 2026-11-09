// The auth service's settings: the environment file's variables (deploy/auth.env.example), each
// overridden by its command-line option.
#pragma once

#include <cstdint>
#include <string>

#include "Log.h"

namespace sfcop::auth {

struct Endpoint {
    std::string address;  // dotted quad
    std::uint16_t port = 0;
};

struct Options {
    Endpoint login{"0.0.0.0", 29900};    // SFCOP_LOGIN_LISTEN, --login-listen
    Endpoint search{"0.0.0.0", 29901};   // SFCOP_SEARCH_LISTEN, --search-listen
    // SFCOP_PATCH_LISTEN, --patch-listen; "off" serves no version check (the client then reads
    // the check as current anyway).
    Endpoint patch{"0.0.0.0", 80};
    bool patchOff = false;
    Endpoint health{"127.0.0.1", 47714};  // SFCOP_HEALTH_LISTEN, --health-listen
    LogLevel logLevel = LogLevel::kInfo;  // SFCOP_LOG_LEVEL, --log-level
    bool help = false;
};

// `getenv` is a seam for tests. False names the bad variable in `error`.
bool applyEnvironment(Options& out, const char* (*getenv)(const char*), std::string& error);
bool parseArgs(int argc, const char* const* argv, Options& out, std::string& error);
bool parseListen(const std::string& text, Endpoint& out);

}  // namespace sfcop::auth
