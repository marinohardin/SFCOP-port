#include "Config.h"

#include <cstdlib>

namespace sfcop::lobby {

bool parseIPv4(const std::string& text, uint32_t& out) {
    uint32_t value = 0;
    int parts = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t dot = text.find('.', pos);
        std::string piece = text.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
        if (piece.empty() || piece.size() > 3) return false;
        uint32_t v = 0;
        for (char c : piece) {
            if (c < '0' || c > '9') return false;
            v = v * 10 + uint32_t(c - '0');
        }
        if (v > 255) return false;
        value = (value << 8) | v;
        ++parts;
        if (dot == std::string::npos) break;
        pos = dot + 1;
    }
    if (parts != 4) return false;
    out = value;
    return true;
}

bool parseListenAddress(const std::string& text, ListenAddress& out) {
    size_t colon = text.rfind(':');
    if (colon == std::string::npos) return false;
    uint32_t ignored = 0;
    std::string host = text.substr(0, colon);
    std::string port = text.substr(colon + 1);
    if (!parseIPv4(host, ignored) || port.empty() || port.size() > 5) return false;
    unsigned long p = 0;
    for (char c : port) {
        if (c < '0' || c > '9') return false;
        p = p * 10 + unsigned(c - '0');
    }
    if (p > 65535) return false;
    out.host = host;
    out.port = uint16_t(p);
    return true;
}

std::optional<ServiceConfig> loadConfig(const GetEnv& getenv, std::string& error) {
    ServiceConfig c;
    auto get = [&](const char* name) -> std::string {
        const char* v = getenv(name);
        return v ? v : "";
    };

    std::string key = get("SFCOP_TICKET_KEY");
    if (key.size() != 64) {
        error = "SFCOP_TICKET_KEY must be 64 hexadecimal digits";
        return std::nullopt;
    }
    for (size_t i = 0; i < 32; ++i) {
        int v[2];
        for (int k = 0; k < 2; ++k) {
            char ch = key[i * 2 + size_t(k)];
            if (ch >= '0' && ch <= '9') v[k] = ch - '0';
            else if (ch >= 'a' && ch <= 'f') v[k] = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') v[k] = ch - 'A' + 10;
            else {
                error = "SFCOP_TICKET_KEY must be 64 hexadecimal digits";
                return std::nullopt;
            }
        }
        c.ticketKey[i] = uint8_t(v[0] * 16 + v[1]);
    }
    bool allZero = true;
    for (uint8_t b : c.ticketKey) allZero = allZero && b == 0;
    if (allZero) {
        error = "SFCOP_TICKET_KEY is all zeros; generate one with: openssl rand -hex 32";
        return std::nullopt;
    }

    struct Named {
        const char* name;
        ListenAddress* target;
    } listens[] = {{"SFCOP_HTTP_LISTEN", &c.http},
                   {"SFCOP_HEALTH_LISTEN", &c.health}};
    for (const Named& n : listens) {
        std::string v = get(n.name);
        if (v.empty()) continue;
        if (!parseListenAddress(v, *n.target)) {
            error = std::string(n.name) + " must be a dotted quad, a colon and a port";
            return std::nullopt;
        }
    }
    std::string host = get("SFCOP_RENDEZVOUS_HOST");
    if (host.empty() || host == "lobby.example.test") {
        error = "SFCOP_RENDEZVOUS_HOST is not set to the relay's public name; edit /etc/sfcop/lobby.env";
        return std::nullopt;
    }
    c.rendezvousHost = host;
    struct Port {
        const char* name;
        uint16_t* target;
    } ports[] = {{"SFCOP_RELAY_PORT", &c.rendezvousPort}, {"SFCOP_RELAY_HEALTH_PORT", &c.relayHealthPort}};
    for (const Port& pt : ports) {
        std::string v = get(pt.name);
        if (v.empty()) continue;
        char* end = nullptr;
        unsigned long p = std::strtoul(v.c_str(), &end, 10);
        if (*end != '\0' || p == 0 || p > 65535) {
            error = std::string(pt.name) + " must be a port number";
            return std::nullopt;
        }
        *pt.target = uint16_t(p);
    }

    std::string blocklist = get("SFCOP_BLOCKLIST");
    size_t pos = 0;
    while (pos < blocklist.size()) {
        size_t comma = blocklist.find(',', pos);
        std::string item = blocklist.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        pos = comma == std::string::npos ? blocklist.size() : comma + 1;
        while (!item.empty() && item.front() == ' ') item.erase(item.begin());
        while (!item.empty() && item.back() == ' ') item.pop_back();
        if (item.empty()) continue;
        uint32_t ignored = 0;
        if (!parseIPv4(item, ignored)) {
            error = "SFCOP_BLOCKLIST holds something that is not an IPv4 address";
            return std::nullopt;
        }
        c.blocked.insert(item);
    }
    return c;
}

} // namespace sfcop::lobby
