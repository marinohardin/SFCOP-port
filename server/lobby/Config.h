// The service's configuration, read from environment variables (the host's environment file,
// outside the repository: server/README.md lists every name).
#pragma once

#include "Ticket.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>

namespace sfcop::lobby {

struct ListenAddress {
    std::string host; // dotted quad
    uint16_t port = 0;
};

struct ServiceConfig {
    ListenAddress http{"127.0.0.1", 8080};        // the lobby API, behind the host's TLS proxy
    ListenAddress health{"127.0.0.1", 47712};     // the same API on loopback, for the host's monitor
    uint16_t relayHealthPort = 47711;             // the relay's loopback port, for the removal call
    std::string rendezvousHost = "localhost";     // ServiceAddress.host in the answers: the relay's name
    uint16_t rendezvousPort = 47710;              // ServiceAddress.port: the relay's UDP port
    TicketKey ticketKey{};
    std::set<std::string> blocked;                // IPv4 text
};

using GetEnv = std::function<const char*(const char* name)>;

// nullopt with `error` set when a value is malformed or the ticket key is missing. Names:
// SFCOP_TICKET_KEY (64 hex digits, required; shared with the relay), SFCOP_HTTP_LISTEN,
// SFCOP_HEALTH_LISTEN (host:port), SFCOP_RENDEZVOUS_HOST (the relay's name, told to clients),
// SFCOP_RELAY_PORT (its UDP port, default 47710), SFCOP_RELAY_HEALTH_PORT (its loopback HTTP
// port, default 47711), SFCOP_BLOCKLIST (comma-separated IPv4 addresses).
std::optional<ServiceConfig> loadConfig(const GetEnv& getenv, std::string& error);

bool parseListenAddress(const std::string& text, ListenAddress& out);
// "a.b.c.d" to the host-order value; false when it is not a dotted quad.
bool parseIPv4(const std::string& text, uint32_t& out);

} // namespace sfcop::lobby
