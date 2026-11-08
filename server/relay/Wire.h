// The rendezvous messages the service speaks and the envelope header it reads
// (src/net/RENDEZVOUS.md, PROTOCOL.md). Little-endian; every decoder checks bounds and refuses a
// datagram that is short, has trailing bytes or a count over its limit.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sfcop::relay {

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint32_t kRvMagic = 0x56524653;        // "SFRV"
constexpr std::uint32_t kEnvelopeMagic = 0x454E4653;  // "SFNE"
constexpr std::uint8_t kRvVersion = 1;
constexpr std::uint8_t kEnvelopeVersion = 1;
constexpr std::size_t kEnvelopeHeaderSize = 32;
constexpr std::size_t kMaxEnvelopeSize = 1200;
constexpr std::size_t kRvHeaderSize = 12;
constexpr std::size_t kTicketSize = 36;
constexpr std::size_t kMaxBindCandidates = 8;

enum RvType : std::uint8_t {
    kBind = 1,
    kBindReply = 2,
    kConnect = 3,
    kPeerCandidates = 4,
    kProbe = 5,
    kProbeReply = 6,
    kUnbind = 7,
    kRvError = 8,
};

enum RvResult : std::uint32_t {
    kOk = 0,
    kBadTicket = 1,
    kTicketExpired = 2,
    kUnknownSession = 3,
    kUnknownPeer = 4,
    kUnknownBinding = 5,
    kRateLimited = 6,
    kUnsupportedVersion = 7,
    kFull = 8,
};

enum CandidateKind : std::uint8_t { kLocal = 1, kReflexive = 2 };

struct Candidate {
    std::uint32_t address = 0;  // IPv4, host-order value
    std::uint16_t port = 0;
    std::uint8_t kind = 0;
    bool operator==(const Candidate& o) const {
        return address == o.address && port == o.port && kind == o.kind;
    }
};

struct RvHeader {
    std::uint8_t version = 0;
    std::uint8_t type = 0;
    std::uint32_t transaction = 0;
};

struct Ticket {
    std::uint64_t sessionId = 0;
    std::uint32_t peerId = 0;
    std::uint32_t expiresAt = 0;
    std::uint8_t role = 0;
    std::uint8_t mac[16] = {};
};

struct BindMessage {
    RvHeader header;
    Ticket ticket;
    std::vector<Candidate> candidates;
};

struct ConnectMessage {
    RvHeader header;
    std::uint64_t sessionId = 0;
    std::uint32_t source = 0;
    std::uint32_t target = 0;
};

struct UnbindMessage {
    RvHeader header;
    std::uint64_t sessionId = 0;
    std::uint32_t peer = 0;
};

struct EnvelopeHeader {
    std::uint8_t version = 0;
    std::uint8_t kind = 0;
    std::uint64_t sessionId = 0;
    std::uint32_t source = 0;
    std::uint32_t destination = 0;
};

// The 4 bytes at the start of a datagram, or 0 when it is shorter.
std::uint32_t peekMagic(const std::uint8_t* data, std::size_t size);

bool decodeRvHeader(const std::uint8_t* data, std::size_t size, RvHeader& out);
bool decodeBind(const std::uint8_t* data, std::size_t size, BindMessage& out);
bool decodeConnect(const std::uint8_t* data, std::size_t size, ConnectMessage& out);
bool decodeUnbind(const std::uint8_t* data, std::size_t size, UnbindMessage& out);
// Reads the 32-byte header only; the payload is never looked at.
bool decodeEnvelopeHeader(const std::uint8_t* data, std::size_t size, EnvelopeHeader& out);

// The 20 bytes the ticket's mac covers.
void encodeTicketBody(const Ticket& t, std::uint8_t out[20]);
Bytes encodeTicket(const Ticket& t);
Bytes encodeBind(const BindMessage& m);  // for tests and clients
Bytes encodeConnect(const ConnectMessage& m);
Bytes encodeUnbind(const UnbindMessage& m);
Bytes encodeBindReply(std::uint32_t transaction, std::uint32_t result, const Candidate& reflexive,
                      std::uint16_t refreshSeconds);
Bytes encodeRvError(std::uint32_t transaction, std::uint32_t result);
Bytes encodePeerCandidates(std::uint32_t transaction, std::uint64_t sessionId, std::uint32_t peer,
                           std::uint32_t punchId, const std::vector<Candidate>& candidates);

}  // namespace sfcop::relay
