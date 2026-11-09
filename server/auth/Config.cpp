#include "Config.h"

namespace sfcop::auth {
namespace {

bool dottedQuad(const std::string& text) {
    int parts = 0;
    std::size_t i = 0;
    while (parts < 4) {
        std::size_t start = i;
        unsigned v = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9' && i - start < 3) {
            v = v * 10 + unsigned(text[i] - '0');
            ++i;
        }
        if (i == start || v > 255) return false;
        if (++parts < 4) {
            if (i >= text.size() || text[i] != '.') return false;
            ++i;
        }
    }
    return i == text.size();
}

bool setOne(Options& out, const std::string& name, const std::string& value) {
    if (name == "login-listen") return parseListen(value, out.login);
    if (name == "search-listen") return parseListen(value, out.search);
    if (name == "health-listen") return parseListen(value, out.health);
    if (name == "patch-listen") {
        if (value == "off") {
            out.patchOff = true;
            return true;
        }
        out.patchOff = false;
        return parseListen(value, out.patch);
    }
    if (name == "log-level") return parseLogLevel(value, out.logLevel);
    return false;
}

struct EnvName {
    const char* variable;
    const char* option;
};

constexpr EnvName kEnvironment[] = {
    {"SFCOP_LOGIN_LISTEN", "login-listen"},
    {"SFCOP_SEARCH_LISTEN", "search-listen"},
    {"SFCOP_PATCH_LISTEN", "patch-listen"},
    {"SFCOP_HEALTH_LISTEN", "health-listen"},
    {"SFCOP_LOG_LEVEL", "log-level"},
};

}  // namespace

bool parseListen(const std::string& text, Endpoint& out) {
    std::size_t colon = text.rfind(':');
    if (colon == std::string::npos || !dottedQuad(text.substr(0, colon))) return false;
    std::string portText = text.substr(colon + 1);
    if (portText.empty() || portText.size() > 5) return false;
    unsigned port = 0;
    for (char c : portText) {
        if (c < '0' || c > '9') return false;
        port = port * 10 + unsigned(c - '0');
    }
    if (port == 0 || port > 65535) return false;
    out.address = text.substr(0, colon);
    out.port = std::uint16_t(port);
    return true;
}

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
        std::string name = arg.rfind("--", 0) == 0 ? arg.substr(2) : std::string();
        bool known = false;
        for (const EnvName& e : kEnvironment) known = known || name == e.option;
        if (!known) {
            error = "unknown option " + arg;
            return false;
        }
        if (i + 1 >= argc) {
            error = "missing value after " + arg;
            return false;
        }
        if (!setOne(out, name, argv[++i])) {
            error = "bad " + arg;
            return false;
        }
    }
    return true;
}

}  // namespace sfcop::auth
