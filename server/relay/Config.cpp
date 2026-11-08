#include "Config.h"

#include <cstdlib>
#include <sstream>

namespace sfcop::relay {
namespace {

bool parsePort(const std::string& text, std::uint16_t& out) {
    if (text.empty() || text.size() > 5) return false;
    unsigned v = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + unsigned(c - '0');
    }
    if (v == 0 || v > 65535) return false;
    out = std::uint16_t(v);
    return true;
}

std::string trim(const std::string& s) {
    std::size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return std::string();
    std::size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

}  // namespace

bool parseIPv4(const std::string& text, std::uint32_t& out) {
    std::uint32_t value = 0;
    int parts = 0;
    std::size_t i = 0;
    while (parts < 4) {
        std::size_t start = i;
        unsigned v = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            v = v * 10 + unsigned(text[i] - '0');
            if (v > 255) return false;
            ++i;
        }
        if (i == start || i - start > 3) return false;
        value = (value << 8) | v;
        ++parts;
        if (parts < 4) {
            if (i >= text.size() || text[i] != '.') return false;
            ++i;
        }
    }
    if (i != text.size()) return false;
    out = value;
    return true;
}

bool parseLogLevel(const std::string& text, LogLevel& out) {
    if (text == "debug") out = LogLevel::kDebug;
    else if (text == "info") out = LogLevel::kInfo;
    else if (text == "warn") out = LogLevel::kWarn;
    else if (text == "error") out = LogLevel::kError;
    else return false;
    return true;
}

bool applyEnvironment(Options& out, const char* (*getenv)(const char*), std::string& error) {
    if (const char* v = getenv("SFCOP_RELAY_PORT")) {
        if (!parsePort(v, out.port)) { error = "bad SFCOP_RELAY_PORT"; return false; }
    }
    if (const char* v = getenv("SFCOP_HEALTH_LISTEN")) {  // host:port, as the monitor reads it
        std::string text = v;
        std::size_t colon = text.rfind(':');
        std::uint32_t address;
        if (colon == std::string::npos || !parseIPv4(text.substr(0, colon), address) ||
            !parsePort(text.substr(colon + 1), out.healthPort)) {
            error = "bad SFCOP_HEALTH_LISTEN";
            return false;
        }
        out.healthAddress = text.substr(0, colon);
    }
    if (const char* v = getenv("SFCOP_LOG_LEVEL")) {
        if (!parseLogLevel(v, out.logLevel)) { error = "bad SFCOP_LOG_LEVEL"; return false; }
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
        if (i + 1 >= argc) {
            error = "missing value after " + arg;
            return false;
        }
        std::string value = argv[++i];
        if (arg == "--port") {
            if (!parsePort(value, out.port)) { error = "bad --port"; return false; }
        } else if (arg == "--health-port") {
            if (!parsePort(value, out.healthPort)) { error = "bad --health-port"; return false; }
        } else if (arg == "--bind") {
            std::uint32_t a;
            if (!parseIPv4(value, a)) { error = "bad --bind"; return false; }
            out.bindAddress = value;
        } else if (arg == "--health-bind") {
            std::uint32_t a;
            if (!parseIPv4(value, a)) { error = "bad --health-bind"; return false; }
            out.healthAddress = value;
        } else if (arg == "--blocklist") {
            out.blocklistPath = value;
        } else if (arg == "--log-level") {
            if (!parseLogLevel(value, out.logLevel)) { error = "bad --log-level"; return false; }
        } else {
            error = "unknown option " + arg;
            return false;
        }
    }
    return true;
}

bool parseHexKey(const std::string& text, Bytes& out) {
    if (text.size() != 64) return false;
    Bytes key;
    for (std::size_t i = 0; i < 64; i += 2) {
        int v = 0;
        for (int j = 0; j < 2; ++j) {
            char c = text[i + std::size_t(j)];
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = 10 + c - 'a';
            else if (c >= 'A' && c <= 'F') d = 10 + c - 'A';
            else return false;
            v = v * 16 + d;
        }
        key.push_back(std::uint8_t(v));
    }
    out = key;
    return true;
}

bool parseBlocklist(const std::string& text, std::unordered_set<std::uint32_t>& out,
                    std::string& error) {
    std::unordered_set<std::uint32_t> result;
    std::istringstream in(text);
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        ++number;
        std::size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        line = trim(line);
        if (line.empty()) continue;
        std::uint32_t a;
        if (!parseIPv4(line, a)) {
            error = "line " + std::to_string(number) + " is not an IPv4 address";
            return false;
        }
        result.insert(a);
    }
    out = std::move(result);
    return true;
}

}  // namespace sfcop::relay
