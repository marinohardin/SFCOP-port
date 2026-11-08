// A Transport with a working Rendezvous (Transport.h, "Phase 3"), made by wrapping a backend's
// own Transport. Every socket the backend opens passes through unchanged; the sockets that
// ConnectToPeer() and AcceptPeer() hand out are the wrapper's own. Each talks to one peer and
// carries one session message per Send() and Receive(): the envelope (PROTOCOL.md) is added and
// taken off underneath, and every peer's envelopes share the one UDP socket of the binding, so
// that they leave through the NAT mapping the rendezvous service saw (RENDEZVOUS.md, "Bind").
//
// The rendezvous client is RendezvousEngine; this class gives it a thread and a clock. The thread
// starts at the first Bind(), runs the engine every 10 ms or when a datagram arrives, and stops
// when the transport is destroyed; it keeps the binding refreshed and every path alive.
//
// Socket ids with bit 31 set are the wrapper's; the backend's ids stay below it (TransportHost
// numbers its sockets from 1).
#pragma once

#include "platform/Transport/Transport.h"

#include <cstdint>
#include <memory>
#include <string>

namespace sfcop::net {

// Where the rendezvous service is, and the client's switches.
struct RendezvousConfig {
    std::string serviceHost; // a host name or dotted quad; empty: none is configured
    std::uint16_t servicePort = 47710;
    bool forceRelay = false;     // RENDEZVOUS.md step 7: no probes, the relay at once
    bool localCandidates = true; // offer this machine's interface addresses
};

// The configuration in the environment: SFCOP_RENDEZVOUS ("host" or "host:port") and
// SFCOP_RENDEZVOUS_FORCE_RELAY ("1" forces the relay). Unset: no service, and GetRendezvous()
// answers kUnsupported.
RendezvousConfig rendezvousConfigFromEnvironment();

// Parses "host" or "host:port" into `config`; false (and `config` unchanged) when malformed.
bool parseServiceAddress(const std::string &text, RendezvousConfig &config);

std::unique_ptr<platform::Transport>
makeRendezvousTransport(std::unique_ptr<platform::Transport> inner, RendezvousConfig config);

// How many times the service thread of a transport from makeRendezvousTransport() has pumped
// its engine; 0 for any other Transport. For tests: the thread parks while there is nothing to
// pump.
std::uint64_t rendezvousPumps(platform::Transport &transport);

} // namespace sfcop::net
