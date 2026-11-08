// The lobby's one call to the relay: when a session leaves the table, the relay drops its
// bindings. The relay (sfcop-relay, N4) answers on loopback only:
//   DELETE /v1/bindings/<session>   <session> = 16 lowercase hex digits, no body
//   204 whether or not the session had bindings; 400 for a bad id; 405, 404 otherwise.
// A relay outage never blocks the lobby: a failed call is logged at debug and dropped.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace sfcop::lobby {

class RelayClient {
public:
    virtual ~RelayClient() = default;
    virtual void removeSession(uint64_t sessionId) = 0;
};

// Sends one request and returns the whole answer, or nullopt when the relay cannot be reached.
using RelayTransport = std::function<std::optional<std::string>(const std::string& request)>;

class HttpRelayClient : public RelayClient {
public:
    explicit HttpRelayClient(RelayTransport transport) : transport_(std::move(transport)) {}
    void removeSession(uint64_t sessionId) override;

private:
    RelayTransport transport_;
};

// The exact request bytes for a removal.
std::string relayRemovalRequest(uint64_t sessionId);
// The status of an HTTP answer, or 0 when it is not one.
int httpStatusOf(const std::string& response);

// A transport that connects to 127.0.0.1:<port>, waits at most 100 ms in all, and returns as soon as
// the answer's status line has arrived, without waiting for the relay to close.
RelayTransport loopbackTransport(uint16_t port);

} // namespace sfcop::lobby
