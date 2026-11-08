// The rendezvous messages of RENDEZVOUS.md, "Messages": bind, connect, candidates, probes,
// unbind and errors, and the ticket's layout. Encoding and decoding only; the client that sends
// them is RendezvousEngine, the service is N4's.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace sfcop::net {

constexpr std::uint32_t kRendezvousMagic = 0x56524653; // "SFRV" as bytes
constexpr std::uint8_t kRendezvousVersion = 1;
constexpr std::uint16_t kRendezvousPort = 47710;
constexpr std::size_t kRendezvousHeaderBytes = 12;
constexpr std::size_t kTicketBytes = 36;
constexpr std::size_t kMaxBindCandidates = 8;
constexpr std::size_t kMaxPeerCandidates = 9;

enum class RvType : std::uint8_t {
    kBind = 1,
    kBindReply = 2,
    kConnect = 3,
    kPeerCandidates = 4,
    kProbe = 5,
    kProbeReply = 6,
    kUnbind = 7,
    kError = 8,
};

// RENDEZVOUS.md, "Results".
enum class RvResult : std::uint32_t {
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

enum class CandidateKind : std::uint8_t { kLocal = 1, kReflexive = 2 };

struct Candidate {
    std::uint32_t address = 0; // host order, as platform::Endpoint
    std::uint16_t port = 0;
    CandidateKind kind = CandidateKind::kLocal;

    friend bool operator==(const Candidate &a, const Candidate &b) {
        return a.address == b.address && a.port == b.port && a.kind == b.kind;
    }
};

using Ticket = std::array<std::uint8_t, kTicketBytes>;

// The ticket's fields; clients never read them (the ticket is opaque to them), the service does.
struct TicketFields {
    std::uint64_t sessionId = 0;
    std::uint32_t peerId = 0;
    std::uint32_t expiresAt = 0; // Unix seconds
    std::uint8_t role = 0;       // 1 host, 2 joiner
    std::array<std::uint8_t, 16> mac{};
};

TicketFields readTicket(const Ticket &ticket);
// The ticket's bytes 0 to 19 (the part the mac covers) and the mac, as given.
Ticket writeTicket(const TicketFields &fields);

// A decoded rendezvous message. Only the fields of its type are meaningful.
struct RvMessage {
    RvType type = RvType::kBind;
    std::uint32_t transaction = 0;
    Ticket ticket{};                   // kBind
    std::vector<Candidate> candidates; // kBind (0 to 8, local), kPeerCandidates (1 to 9)
    RvResult result = RvResult::kOk;   // kBindReply, kError
    Candidate reflexive;               // kBindReply
    std::uint16_t refreshSeconds = 0;  // kBindReply
    std::uint64_t sessionId = 0;       // kConnect, kPeerCandidates, kProbe, kProbeReply, kUnbind
    std::uint32_t source = 0;          // kConnect, kProbe, kProbeReply
    std::uint32_t target = 0;          // kConnect, kProbe, kProbeReply
    std::uint32_t peer = 0;            // kPeerCandidates, kUnbind
    std::uint32_t punchId = 0;         // kPeerCandidates, kProbe, kProbeReply
};

std::vector<std::uint8_t> encodeRendezvous(const RvMessage &message);

// The message in `data`, or nothing when it is malformed: a wrong magic or version, an unknown
// type or candidate kind, a candidate count over its limit (or 0 in kPeerCandidates), a result
// code past kFull, or a size other than the type's. Reserved fields are ignored.
std::optional<RvMessage> decodeRendezvous(const std::uint8_t *data, std::size_t bytes);

// The version byte of a datagram that carries the rendezvous magic, or nothing; the service uses
// it to answer kUnsupportedVersion.
std::optional<std::uint8_t> rendezvousVersion(const std::uint8_t *data, std::size_t bytes);

} // namespace sfcop::net
