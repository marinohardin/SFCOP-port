// The directory's settings: the environment file's variables (deploy/directory.env.example), each
// overridden by its command-line option.
#pragma once

#include <cstdint>
#include <string>

#include "Log.h"

namespace sfcop::directory {

struct Endpoint {
    std::string address;  // dotted quad
    std::uint16_t port = 0;
};

struct Options {
    Endpoint listen{"0.0.0.0", 15101};          // SFCOP_DIRECTORY_LISTEN, --listen
    Endpoint detectorListen{"0.0.0.0", 15102};  // SFCOP_DETECTOR_LISTEN, --detector-listen
    std::string publicAddress;                  // SFCOP_PUBLIC_ADDRESS, --public-address (required)
    std::string campaignFile = "/etc/sfcop/campaigns.txt";  // SFCOP_CAMPAIGN_FILE, --campaigns
    std::string campaignPath = "/StarFleetCommand2/Game/Release";  // SFCOP_CAMPAIGN_PATH
    Endpoint health{"127.0.0.1", 47713};        // SFCOP_HEALTH_LISTEN, --health-listen
    LogLevel logLevel = LogLevel::kInfo;        // SFCOP_LOG_LEVEL, --log-level
    bool help = false;
};

// `getenv` is a seam for tests. False names the bad variable in `error`.
bool applyEnvironment(Options& out, const char* (*getenv)(const char*), std::string& error);
bool parseArgs(int argc, const char* const* argv, Options& out, std::string& error);

// "a.b.c.d:port" with a port 1..65535 (0 allowed when `allowZero`, for tests).
bool parseListen(const std::string& text, Endpoint& out, bool allowZero = false);
bool parseDottedQuad(const std::string& text, std::uint32_t& out);

}  // namespace sfcop::directory
