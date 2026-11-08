// The 36-byte ticket of RENDEZVOUS.md, "Tickets": session, peer, expiry, role and the first 16
// bytes of HMAC-SHA-256 over the first 20 bytes with the key the lobby and the relay share.
#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace sfcop::lobby {

constexpr size_t kTicketSize = 36;
constexpr uint8_t kRoleHost = 1;
constexpr uint8_t kRoleJoiner = 2;
constexpr uint32_t kTicketLifetimeSeconds = 600;

using TicketKey = std::array<uint8_t, 32>;
using TicketBytes = std::array<uint8_t, kTicketSize>;

struct TicketBody {
    uint64_t sessionId = 0;
    uint32_t peerId = 0;
    uint32_t expiresAt = 0; // Unix time, seconds
    uint8_t role = 0;
};

TicketBytes makeTicket(const TicketKey& key, const TicketBody& body);

// The body of a ticket whose size is right, whose reserved bytes are zero, whose role is 1 or 2
// and whose mac is right. Expiry is the caller's business.
std::optional<TicketBody> checkTicket(const TicketKey& key, const uint8_t* bytes, size_t size);

} // namespace sfcop::lobby
