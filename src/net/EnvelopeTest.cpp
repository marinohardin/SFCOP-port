#include "net/Envelope.h"

#include <doctest/doctest.h>

#include <vector>

namespace sfcop::net {
namespace {

using Bytes = std::vector<std::uint8_t>;

// The header of PROTOCOL.md for session 5346434f50000001, peer 2 to peer 1, sequence 7, written
// out by hand; `kind` and `flags` vary, index 0 and count 1 unless given.
Bytes header(std::uint8_t kind, std::uint8_t flags, std::uint16_t index = 0,
             std::uint16_t count = 1) {
    return Bytes{0x53,
                 0x46,
                 0x4E,
                 0x45, // magic "SFNE"
                 0x01,
                 kind,
                 flags,
                 0x00, // version, kind, flags, reserved
                 0x01,
                 0x00,
                 0x00,
                 0x50,
                 0x4F,
                 0x43,
                 0x46,
                 0x53, // sessionId
                 0x02,
                 0x00,
                 0x00,
                 0x00, // source 2
                 0x01,
                 0x00,
                 0x00,
                 0x00, // destination 1
                 0x07,
                 0x00,
                 0x00,
                 0x00, // sequence 7
                 static_cast<std::uint8_t>(index),
                 static_cast<std::uint8_t>(index >> 8),
                 static_cast<std::uint8_t>(count),
                 static_cast<std::uint8_t>(count >> 8)};
}

Bytes concat(Bytes a, const Bytes &b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

EnvelopeHeader expectedHeader(EnvelopeKind kind, std::uint8_t flags = 0) {
    EnvelopeHeader h;
    h.kind = kind;
    h.flags = flags;
    h.sessionId = 0x5346434F50000001ull;
    h.source = 2;
    h.destination = 1;
    h.sequence = 7;
    return h;
}

void checkHeader(const EnvelopeHeader &got, const EnvelopeHeader &want) {
    CHECK(got.kind == want.kind);
    CHECK(got.flags == want.flags);
    CHECK(got.sessionId == want.sessionId);
    CHECK(got.source == want.source);
    CHECK(got.destination == want.destination);
    CHECK(got.sequence == want.sequence);
    CHECK(got.fragmentIndex == want.fragmentIndex);
    CHECK(got.fragmentCount == want.fragmentCount);
}

std::optional<Envelope> decode(const Bytes &bytes) {
    return decodeEnvelope(bytes.data(), bytes.size());
}

} // namespace

TEST_CASE("EnvelopeData encodes and decodes the bytes PROTOCOL.md gives") {
    const Bytes wire = concat(header(1, 0x00), {0x41, 0x42});
    Envelope envelope;
    envelope.header = expectedHeader(EnvelopeKind::kData);
    envelope.payload = {0x41, 0x42};
    CHECK(encodeEnvelope(envelope) == wire);

    std::optional<Envelope> got = decode(wire);
    REQUIRE(got);
    checkHeader(got->header, expectedHeader(EnvelopeKind::kData));
    CHECK(got->payload == Bytes{0x41, 0x42});

    // A fragment: the encoder sets kFragment from the count.
    const Bytes fragment = concat(header(1, 0x03, 2, 3), {0x99});
    Envelope second;
    second.header = expectedHeader(EnvelopeKind::kData, kEnvelopeReliable);
    second.header.fragmentIndex = 2;
    second.header.fragmentCount = 3;
    second.payload = {0x99};
    CHECK(encodeEnvelope(second) == fragment);
    std::optional<Envelope> back = decode(fragment);
    REQUIRE(back);
    CHECK(back->header.flags == 0x03);
    CHECK(back->header.fragmentIndex == 2);
    CHECK(back->header.fragmentCount == 3);
}

TEST_CASE("EnvelopeData is refused empty, oversized, or with a fragment field out of its rules") {
    CHECK_FALSE(decode(header(1, 0x00)));                            // no payload
    CHECK_FALSE(decode(concat(header(1, 0x00), Bytes(1169, 0xAA)))); // 1169 > 1168
    CHECK(decode(concat(header(1, 0x00), Bytes(1168, 0xAA))));       // 1200 on the wire
    CHECK_FALSE(decode(concat(header(1, 0x00, 0, 2), {0x01})));      // count 2 without kFragment
    CHECK_FALSE(decode(concat(header(1, 0x02, 0, 1), {0x01})));      // kFragment with count 1
    CHECK_FALSE(decode(concat(header(1, 0x02, 2, 2), {0x01})));      // index at the count
    CHECK_FALSE(decode(concat(header(1, 0x02, 0, 65), {0x01})));     // count over 64
    CHECK_FALSE(decode(concat(header(1, 0x00, 0, 0), {0x01})));      // count 0
}

TEST_CASE("EnvelopeAck encodes and decodes the bytes PROTOCOL.md gives") {
    const Bytes wire = concat(header(2, 0x00), {0x05, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00});
    Envelope envelope;
    envelope.header = expectedHeader(EnvelopeKind::kAck);
    envelope.cumulative = 5;
    envelope.mask = 3;
    CHECK(encodeEnvelope(envelope) == wire);
    std::optional<Envelope> got = decode(wire);
    REQUIRE(got);
    checkHeader(got->header, expectedHeader(EnvelopeKind::kAck));
    CHECK(got->cumulative == 5);
    CHECK(got->mask == 3);
    // Malformed: one byte short.
    CHECK_FALSE(decode(Bytes(wire.begin(), wire.end() - 1)));
}

TEST_CASE("EnvelopePing and EnvelopePong encode and decode the bytes PROTOCOL.md gives") {
    const Bytes body = {0x44, 0x33, 0x22, 0x11, 0xE8, 0x03, 0x00, 0x00};
    for (std::uint8_t kind : {std::uint8_t{3}, std::uint8_t{4}}) {
        const Bytes wire = concat(header(kind, 0x00), body);
        Envelope envelope;
        envelope.header = expectedHeader(static_cast<EnvelopeKind>(kind));
        envelope.nonce = 0x11223344;
        envelope.sentMs = 1000;
        CHECK(encodeEnvelope(envelope) == wire);
        std::optional<Envelope> got = decode(wire);
        REQUIRE(got);
        CHECK(got->header.kind == static_cast<EnvelopeKind>(kind));
        CHECK(got->nonce == 0x11223344u);
        CHECK(got->sentMs == 1000u);
    }
    // Malformed ping: a fragment count on a kind that has none.
    CHECK_FALSE(decode(concat(header(3, 0x02, 0, 2), body)));
    // Malformed pong: a trailing byte.
    CHECK_FALSE(decode(concat(concat(header(4, 0x00), body), {0x00})));
}

TEST_CASE("EnvelopeClose encodes and decodes the bytes PROTOCOL.md gives") {
    const Bytes wire = concat(header(5, 0x01), {0x02, 0x00, 0x00, 0x00});
    Envelope envelope;
    envelope.header = expectedHeader(EnvelopeKind::kClose, kEnvelopeReliable);
    envelope.reason = 2;
    CHECK(encodeEnvelope(envelope) == wire);
    std::optional<Envelope> got = decode(wire);
    REQUIRE(got);
    checkHeader(got->header, expectedHeader(EnvelopeKind::kClose, kEnvelopeReliable));
    CHECK(got->reason == 2);
    // Malformed: an unknown flag bit.
    CHECK_FALSE(decode(concat(header(5, 0x05), {0x02, 0x00, 0x00, 0x00})));
}

TEST_CASE("an envelope with a wrong magic, version or kind is refused; the relay's peek is lax") {
    Bytes wrongMagic = concat(header(3, 0x00), Bytes(8, 0));
    wrongMagic[0] = 0x54;
    CHECK_FALSE(decode(wrongMagic));
    CHECK_FALSE(peekEnvelopeHeader(wrongMagic.data(), wrongMagic.size()));

    Bytes wrongVersion = concat(header(3, 0x00), Bytes(8, 0));
    wrongVersion[4] = 0x02;
    CHECK_FALSE(decode(wrongVersion));
    CHECK_FALSE(peekEnvelopeHeader(wrongVersion.data(), wrongVersion.size()));

    // An unknown kind is refused by the receiver; the relay reads the header only and forwards.
    const Bytes unknownKind = concat(header(6, 0x00), Bytes(8, 0));
    CHECK_FALSE(decode(unknownKind));
    std::optional<EnvelopeHeader> peeked =
        peekEnvelopeHeader(unknownKind.data(), unknownKind.size());
    REQUIRE(peeked);
    CHECK(peeked->sessionId == 0x5346434F50000001ull);
    CHECK(peeked->source == 2);
    CHECK(peeked->destination == 1);

    const Bytes full = header(1, 0);
    const Bytes shortHeader(full.begin(), full.begin() + 31);
    CHECK_FALSE(peekEnvelopeHeader(shortHeader.data(), shortHeader.size()));
    CHECK_FALSE(decode(shortHeader));
}

} // namespace sfcop::net
