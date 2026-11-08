#include "net/RendezvousMessages.h"

#include <doctest/doctest.h>

#include <vector>

namespace sfcop::net {
namespace {

using Bytes = std::vector<std::uint8_t>;

// RvHeader with transaction 0x0A0B0C0D, written out by hand.
Bytes header(std::uint8_t type) {
    return Bytes{0x53, 0x46, 0x52, 0x56, 0x01, type, 0x00, 0x00, 0x0D, 0x0C, 0x0B, 0x0A};
}

Bytes concat(Bytes a, const Bytes &b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

const Bytes kSession = {0x01, 0x00, 0x00, 0x50, 0x4F, 0x43, 0x46, 0x53}; // 5346434f50000001
const Bytes kPeer1 = {0x01, 0x00, 0x00, 0x00};
const Bytes kPeer2 = {0x02, 0x00, 0x00, 0x00};
const Bytes kPunch = {0xEF, 0xBE, 0xAD, 0xDE};
// 198.51.100.1:40000, reflexive; 192.0.2.20:50000, local.
const Bytes kReflexive = {0x01, 0x64, 0x33, 0xC6, 0x40, 0x9C, 0x02, 0x00};
const Bytes kLocal = {0x14, 0x02, 0x00, 0xC0, 0x50, 0xC3, 0x01, 0x00};
constexpr std::uint64_t kSessionId = 0x5346434F50000001ull;

Bytes ticketBytes() {
    Bytes ticket;
    for (int i = 0; i < 36; ++i) {
        ticket.push_back(static_cast<std::uint8_t>(i));
    }
    return ticket;
}

std::optional<RvMessage> decode(const Bytes &bytes) {
    return decodeRendezvous(bytes.data(), bytes.size());
}

RvMessage base(RvType type) {
    RvMessage message;
    message.type = type;
    message.transaction = 0x0A0B0C0D;
    return message;
}

} // namespace

TEST_CASE("Bind encodes and decodes the bytes RENDEZVOUS.md gives") {
    const Bytes wire = concat(concat(concat(header(1), ticketBytes()), {0x01, 0x00}), kLocal);
    RvMessage bind = base(RvType::kBind);
    for (int i = 0; i < 36; ++i) {
        bind.ticket[i] = static_cast<std::uint8_t>(i);
    }
    bind.candidates = {Candidate{0xC0000214, 50000, CandidateKind::kLocal}};
    CHECK(encodeRendezvous(bind) == wire);
    std::optional<RvMessage> got = decode(wire);
    REQUIRE(got);
    CHECK(got->type == RvType::kBind);
    CHECK(got->transaction == 0x0A0B0C0Du);
    CHECK(got->ticket == bind.ticket);
    REQUIRE(got->candidates.size() == 1);
    CHECK(got->candidates[0] == bind.candidates[0]);
    // No candidates is the minimum, 50 bytes.
    CHECK(decode(concat(concat(header(1), ticketBytes()), {0x00, 0x00})));

    // Malformed: nine candidates (at most 8); a candidate of kind 3; a count with no candidate.
    Bytes nine = concat(concat(header(1), ticketBytes()), {0x09, 0x00});
    for (int i = 0; i < 9; ++i) {
        nine = concat(nine, kLocal);
    }
    CHECK_FALSE(decode(nine));
    Bytes kind3 = wire;
    kind3[56] = 0x03;
    CHECK_FALSE(decode(kind3));
    CHECK_FALSE(decode(concat(concat(header(1), ticketBytes()), {0x01, 0x00})));
}

TEST_CASE("BindReply encodes and decodes the bytes RENDEZVOUS.md gives") {
    const Bytes wire = concat(concat(concat(header(2), {0x00, 0x00, 0x00, 0x00}), kReflexive),
                              {0x0F, 0x00, 0x00, 0x00});
    RvMessage reply = base(RvType::kBindReply);
    reply.result = RvResult::kOk;
    reply.reflexive = Candidate{0xC6336401, 40000, CandidateKind::kReflexive};
    reply.refreshSeconds = 15;
    CHECK(encodeRendezvous(reply) == wire);
    std::optional<RvMessage> got = decode(wire);
    REQUIRE(got);
    CHECK(got->result == RvResult::kOk);
    CHECK(got->reflexive == reply.reflexive);
    CHECK(got->refreshSeconds == 15);
    // Malformed: a result code past kFull.
    Bytes bad = wire;
    bad[12] = 0x09;
    CHECK_FALSE(decode(bad));
}

TEST_CASE("Connect encodes and decodes the bytes RENDEZVOUS.md gives") {
    const Bytes wire = concat(concat(concat(header(3), kSession), kPeer2), kPeer1);
    RvMessage connect = base(RvType::kConnect);
    connect.sessionId = kSessionId;
    connect.source = 2;
    connect.target = 1;
    CHECK(encodeRendezvous(connect) == wire);
    std::optional<RvMessage> got = decode(wire);
    REQUIRE(got);
    CHECK(got->sessionId == kSessionId);
    CHECK(got->source == 2);
    CHECK(got->target == 1);
    // Malformed: one byte short.
    CHECK_FALSE(decode(Bytes(wire.begin(), wire.end() - 1)));
}

TEST_CASE("PeerCandidates encodes and decodes the bytes RENDEZVOUS.md gives") {
    const Bytes wire =
        concat(concat(concat(concat(concat(concat(header(4), kSession), kPeer1), kPunch),
                             {0x02, 0x00, 0x00, 0x00}),
                      kReflexive),
               kLocal);
    RvMessage candidates = base(RvType::kPeerCandidates);
    candidates.sessionId = kSessionId;
    candidates.peer = 1;
    candidates.punchId = 0xDEADBEEF;
    candidates.candidates = {Candidate{0xC6336401, 40000, CandidateKind::kReflexive},
                             Candidate{0xC0000214, 50000, CandidateKind::kLocal}};
    CHECK(encodeRendezvous(candidates) == wire);
    std::optional<RvMessage> got = decode(wire);
    REQUIRE(got);
    CHECK(got->peer == 1);
    CHECK(got->punchId == 0xDEADBEEFu);
    REQUIRE(got->candidates.size() == 2);
    CHECK(got->candidates[1] == candidates.candidates[1]);
    // Malformed: no candidate (at least one).
    CHECK_FALSE(decode(concat(concat(concat(concat(header(4), kSession), kPeer1), kPunch),
                              {0x00, 0x00, 0x00, 0x00})));
}

TEST_CASE("Probe and ProbeReply encode and decode the bytes RENDEZVOUS.md gives") {
    for (std::uint8_t type : {std::uint8_t{5}, std::uint8_t{6}}) {
        const Bytes wire =
            concat(concat(concat(concat(header(type), kSession), kPeer2), kPeer1), kPunch);
        RvMessage probe = base(static_cast<RvType>(type));
        probe.sessionId = kSessionId;
        probe.source = 2;
        probe.target = 1;
        probe.punchId = 0xDEADBEEF;
        CHECK(encodeRendezvous(probe) == wire);
        std::optional<RvMessage> got = decode(wire);
        REQUIRE(got);
        CHECK(got->type == static_cast<RvType>(type));
        CHECK(got->source == 2);
        CHECK(got->target == 1);
        CHECK(got->punchId == 0xDEADBEEFu);
        // Malformed: a trailing byte.
        CHECK_FALSE(decode(concat(wire, {0x00})));
    }
}

TEST_CASE("Unbind and RvError encode and decode the bytes RENDEZVOUS.md gives") {
    const Bytes unbindWire = concat(concat(header(7), kSession), kPeer2);
    RvMessage unbind = base(RvType::kUnbind);
    unbind.sessionId = kSessionId;
    unbind.peer = 2;
    CHECK(encodeRendezvous(unbind) == unbindWire);
    std::optional<RvMessage> gotUnbind = decode(unbindWire);
    REQUIRE(gotUnbind);
    CHECK(gotUnbind->peer == 2);
    CHECK_FALSE(decode(Bytes(unbindWire.begin(), unbindWire.end() - 1)));

    const Bytes errorWire = concat(header(8), {0x04, 0x00, 0x00, 0x00});
    RvMessage error = base(RvType::kError);
    error.result = RvResult::kUnknownPeer;
    CHECK(encodeRendezvous(error) == errorWire);
    std::optional<RvMessage> gotError = decode(errorWire);
    REQUIRE(gotError);
    CHECK(gotError->result == RvResult::kUnknownPeer);
    CHECK_FALSE(decode(concat(header(8), {0x10, 0x00, 0x00, 0x00})));
}

TEST_CASE("a rendezvous datagram with a wrong magic, version or type is refused") {
    const Bytes good = concat(header(8), {0x00, 0x00, 0x00, 0x00});
    Bytes magic = good;
    magic[3] = 0x57;
    CHECK_FALSE(decode(magic));
    CHECK_FALSE(rendezvousVersion(magic.data(), magic.size()));
    Bytes version = good;
    version[4] = 0x02;
    CHECK_FALSE(decode(version));
    REQUIRE(rendezvousVersion(version.data(), version.size()));
    CHECK(*rendezvousVersion(version.data(), version.size()) == 2);
    Bytes type = good;
    type[5] = 0x09;
    CHECK_FALSE(decode(type));
    type[5] = 0x00;
    CHECK_FALSE(decode(type));
}

TEST_CASE("a ticket's fields are at the offsets RENDEZVOUS.md gives") {
    TicketFields fields;
    fields.sessionId = kSessionId;
    fields.peerId = 2;
    fields.expiresAt = 0x6A000000;
    fields.role = 2;
    for (int i = 0; i < 16; ++i) {
        fields.mac[i] = static_cast<std::uint8_t>(0xF0 + i);
    }
    Ticket ticket = writeTicket(fields);
    const Bytes want =
        concat(concat(concat(kSession, kPeer2), {0x00, 0x00, 0x00, 0x6A, 0x02, 0x00, 0x00, 0x00}),
               {0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD,
                0xFE, 0xFF});
    CHECK(Bytes(ticket.begin(), ticket.end()) == want);
    TicketFields back = readTicket(ticket);
    CHECK(back.sessionId == kSessionId);
    CHECK(back.peerId == 2);
    CHECK(back.expiresAt == 0x6A000000u);
    CHECK(back.role == 2);
    CHECK(back.mac == fields.mac);
}

} // namespace sfcop::net
