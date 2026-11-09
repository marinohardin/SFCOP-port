#include "Config.h"

#include "Campaigns.h"

namespace sfcop::directory {

bool parseDottedQuad(const std::string& text, std::uint32_t& out) {
    std::uint32_t address = 0;
    std::uint16_t port = 0;
    if (!parseEndpoint(text + ":1", address, port)) return false;
    out = address;
    return true;
}

bool parseListen(const std::string& text, Endpoint& out, bool allowZero) {
    std::size_t colon = text.rfind(':');
    if (colon == std::string::npos) return false;
    std::string host = text.substr(0, colon);
    std::string portText = text.substr(colon + 1);
    std::uint32_t address = 0;
    if (!parseDottedQuad(host, address) || portText.empty() || portText.size() > 5) return false;
    unsigned port = 0;
    for (char c : portText) {
        if (c < '0' || c > '9') return false;
        port = port * 10 + unsigned(c - '0');
    }
    if (port > 65535 || (port == 0 && !allowZero)) return false;
    out.address = host;
    out.port = std::uint16_t(port);
    return true;
}

namespace {

bool setOne(Options& out, const std::string& name, const std::string& value) {
    if (name == "listen") return parseListen(value, out.listen);
    if (name == "detector-listen") return parseListen(value, out.detectorListen);
    if (name == "health-listen") return parseListen(value, out.health);
    if (name == "public-address") {
        std::uint32_t a = 0;
        if (!parseDottedQuad(value, a) || a == 0) return false;
        out.publicAddress = value;
        return true;
    }
    if (name == "campaigns") {
        if (value.empty()) return false;
        out.campaignFile = value;
        return true;
    }
    if (name == "campaign-path") {
        if (value.empty() || value[0] != '/') return false;
        out.campaignPath = value;
        return true;
    }
    if (name == "log-level") return parseLogLevel(value, out.logLevel);
    return false;
}

struct EnvName {
    const char* variable;
    const char* option;
};

constexpr EnvName kEnvironment[] = {
    {"SFCOP_DIRECTORY_LISTEN", "listen"},
    {"SFCOP_DETECTOR_LISTEN", "detector-listen"},
    {"SFCOP_PUBLIC_ADDRESS", "public-address"},
    {"SFCOP_CAMPAIGN_FILE", "campaigns"},
    {"SFCOP_CAMPAIGN_PATH", "campaign-path"},
    {"SFCOP_HEALTH_LISTEN", "health-listen"},
    {"SFCOP_LOG_LEVEL", "log-level"},
};

}  // namespace

bool applyEnvironment(Options& out, const char* (*getenv)(const char*), std::string& error) {
    for (const EnvName& e : kEnvironment) {
        if (const char* v = getenv(e.variable)) {
            if (!setOne(out, e.option, v)) {
                error = std::string("bad ") + e.variable;
                return false;
            }
        }
    }
    return true;
}

bool parseArgs(int argc, const char* const* argv, Options& out, std::string& error) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            out.help = true;
            continue;
        }
        if (arg.rfind("--", 0) != 0) {
            error = "unknown option " + arg;
            return false;
        }
        if (i + 1 >= argc) {
            error = "missing value after " + arg;
            return false;
        }
        std::string name = arg.substr(2);
        bool known = false;
        for (const EnvName& e : kEnvironment) known = known || name == e.option;
        if (!known) {
            error = "unknown option " + arg;
            return false;
        }
        if (!setOne(out, name, argv[++i])) {
            error = "bad " + arg;
            return false;
        }
    }
    return true;
}

}  // namespace sfcop::directory
