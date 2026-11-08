#include "Wire.h"

#include <cstring>

namespace sfcop::relay {
namespace {

std::uint16_t rd16(const std::uint8_t* p) { return std::uint16_t(p[0] | (p[1] << 8)); }
std::uint32_t rd32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) |
           (std::uint32_t(p[3]) << 24);
}
std::uint64_t rd64(const std::uint8_t* p) {
    return std::uint64_t(rd32(p)) | (std::uint64_t(rd32(p + 4)) << 32);
}

void put8(Bytes& b, std::uint8_t v) { b.push_back(v); }
void put16(Bytes& b, std::uint16_t v) {
    b.push_back(std::uint8_t(v));
    b.push_back(std::uint8_t(v >> 8));
}
void put32(Bytes& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(std::uint8_t(v >> (8 * i)));
}
void put64(Bytes& b, std::uint64_t v) {
    put32(b, std::uint32_t(v));
    put32(b, std::uint32_t(v >> 32));
}

void putHeader(Bytes& b, std::uint8_t type, std::uint32_t transaction) {
    put32(b, kRvMagic);
    put8(b, kRvVersion);
    put8(b, type);
    put16(b, 0);
    put32(b, transaction);
}

void putCandidate(Bytes& b, const Candidate& c) {
    put32(b, c.address);
    put16(b, c.port);
    put8(b, c.kind);
    put8(b, 0);
}

void readTicket(const std::uint8_t* p, Ticket& t) {
    t.sessionId = rd64(p);
    t.peerId = rd32(p + 8);
    t.expiresAt = rd32(p + 12);
    t.role = p[16];
    std::memcpy(t.mac, p + 20, 16);
}

}  // namespace

std::uint32_t peekMagic(const std::uint8_t* data, std::size_t size) {
    return size >= 4 ? rd32(data) : 0;
}

bool decodeRvHeader(const std::uint8_t* data, std::size_t size, RvHeader& out) {
    if (size < kRvHeaderSize || rd32(data) != kRvMagic) return false;
    out.version = data[4];
    out.type = data[5];
    out.transaction = rd32(data + 8);
    return true;
}

bool decodeBind(const std::uint8_t* data, std::size_t size, BindMessage& out) {
    if (size < 50 || !decodeRvHeader(data, size, out.header) || out.header.type != kBind)
        return false;
    std::size_t count = data[48];
    if (count > kMaxBindCandidates || size != 50 + count * 8) return false;
    readTicket(data + 12, out.ticket);
    out.candidates.clear();
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t* p = data + 50 + i * 8;
        out.candidates.push_back({rd32(p), rd16(p + 4), p[6]});
    }
    return true;
}

bool decodeConnect(const std::uint8_t* data, std::size_t size, ConnectMessage& out) {
    if (size != 28 || !decodeRvHeader(data, size, out.header) || out.header.type != kConnect)
        return false;
    out.sessionId = rd64(data + 12);
    out.source = rd32(data + 20);
    out.target = rd32(data + 24);
    return true;
}

bool decodeUnbind(const std::uint8_t* data, std::size_t size, UnbindMessage& out) {
    if (size != 24 || !decodeRvHeader(data, size, out.header) || out.header.type != kUnbind)
        return false;
    out.sessionId = rd64(data + 12);
    out.peer = rd32(data + 20);
    return true;
}

bool decodeEnvelopeHeader(const std::uint8_t* data, std::size_t size, EnvelopeHeader& out) {
    if (size < kEnvelopeHeaderSize || rd32(data) != kEnvelopeMagic) return false;
    out.version = data[4];
    out.kind = data[5];
    out.sessionId = rd64(data + 8);
    out.source = rd32(data + 16);
    out.destination = rd32(data + 20);
    return true;
}

void encodeTicketBody(const Ticket& t, std::uint8_t out[20]) {
    Bytes b;
    put64(b, t.sessionId);
    put32(b, t.peerId);
    put32(b, t.expiresAt);
    put8(b, t.role);
    put8(b, 0);
    put8(b, 0);
    put8(b, 0);
    std::memcpy(out, b.data(), 20);
}

Bytes encodeTicket(const Ticket& t) {
    std::uint8_t body[20];
    encodeTicketBody(t, body);
    Bytes b(36);
    std::memcpy(b.data(), body, 20);
    std::memcpy(b.data() + 20, t.mac, 16);
    return b;
}

Bytes encodeBind(const BindMessage& m) {
    Bytes b;
    putHeader(b, kBind, m.header.transaction);
    Bytes t = encodeTicket(m.ticket);
    b.insert(b.end(), t.begin(), t.end());
    put8(b, std::uint8_t(m.candidates.size()));
    put8(b, 0);
    for (const Candidate& c : m.candidates) putCandidate(b, c);
    return b;
}

Bytes encodeConnect(const ConnectMessage& m) {
    Bytes b;
    putHeader(b, kConnect, m.header.transaction);
    put64(b, m.sessionId);
    put32(b, m.source);
    put32(b, m.target);
    return b;
}

Bytes encodeUnbind(const UnbindMessage& m) {
    Bytes b;
    putHeader(b, kUnbind, m.header.transaction);
    put64(b, m.sessionId);
    put32(b, m.peer);
    return b;
}

Bytes encodeBindReply(std::uint32_t transaction, std::uint32_t result, const Candidate& reflexive,
                      std::uint16_t refreshSeconds) {
    Bytes b;
    putHeader(b, kBindReply, transaction);
    put32(b, result);
    putCandidate(b, reflexive);
    put16(b, refreshSeconds);
    put16(b, 0);
    return b;
}

Bytes encodeRvError(std::uint32_t transaction, std::uint32_t result) {
    Bytes b;
    putHeader(b, kRvError, transaction);
    put32(b, result);
    return b;
}

Bytes encodePeerCandidates(std::uint32_t transaction, std::uint64_t sessionId, std::uint32_t peer,
                           std::uint32_t punchId, const std::vector<Candidate>& candidates) {
    Bytes b;
    putHeader(b, kPeerCandidates, transaction);
    put64(b, sessionId);
    put32(b, peer);
    put32(b, punchId);
    put8(b, std::uint8_t(candidates.size()));
    put8(b, 0);
    put8(b, 0);
    put8(b, 0);
    for (const Candidate& c : candidates) putCandidate(b, c);
    return b;
}

}  // namespace sfcop::relay
