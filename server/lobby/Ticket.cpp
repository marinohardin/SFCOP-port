#include "Ticket.h"

#include "Sha256.h"

#include <cstring>

namespace sfcop::lobby {

namespace {

void putU32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}
void putU64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i));
}
uint32_t getU32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t getU64(const uint8_t* p) { return uint64_t(getU32(p)) | (uint64_t(getU32(p + 4)) << 32); }

} // namespace

TicketBytes makeTicket(const TicketKey& key, const TicketBody& body) {
    TicketBytes t{};
    putU64(t.data(), body.sessionId);
    putU32(t.data() + 8, body.peerId);
    putU32(t.data() + 12, body.expiresAt);
    t[16] = body.role;
    Digest mac = hmacSha256(key.data(), key.size(), t.data(), 20);
    std::memcpy(t.data() + 20, mac.data(), 16);
    return t;
}

std::optional<TicketBody> checkTicket(const TicketKey& key, const uint8_t* bytes, size_t size) {
    if (size != kTicketSize) return std::nullopt;
    if (bytes[17] != 0 || bytes[18] != 0 || bytes[19] != 0) return std::nullopt;
    if (bytes[16] != kRoleHost && bytes[16] != kRoleJoiner) return std::nullopt;
    Digest mac = hmacSha256(key.data(), key.size(), bytes, 20);
    if (!constantTimeEqual(mac.data(), bytes + 20, 16)) return std::nullopt;
    TicketBody body;
    body.sessionId = getU64(bytes);
    body.peerId = getU32(bytes + 8);
    body.expiresAt = getU32(bytes + 12);
    body.role = bytes[16];
    return body;
}

} // namespace sfcop::lobby
