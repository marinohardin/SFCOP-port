// The envelope of PROTOCOL.md, "Envelope": the header every datagram between two native clients
// carries over the Internet, direct or relayed, and its five kinds. Encoding and decoding only;
// the rules (sequence numbers, fragments, acks, keepalive) are PeerLink's.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace sfcop::net {

constexpr std::uint32_t kEnvelopeMagic = 0x454E4653; // "SFNE" as bytes
constexpr std::uint8_t kEnvelopeVersion = 1;
constexpr std::size_t kEnvelopeHeaderBytes = 32;
constexpr std::size_t kMaxEnvelopeBytes = 1200;
constexpr std::size_t kMaxFragmentPayload = kMaxEnvelopeBytes - kEnvelopeHeaderBytes; // 1168
constexpr std::uint16_t kMaxFragments = 64;

enum class EnvelopeKind : std::uint8_t {
    kData = 1,
    kAck = 2,
    kPing = 3,
    kPong = 4,
    kClose = 5,
};

constexpr std::uint8_t kEnvelopeReliable = 0x01;
constexpr std::uint8_t kEnvelopeFragment = 0x02;

struct EnvelopeHeader {
    EnvelopeKind kind = EnvelopeKind::kData;
    std::uint8_t flags = 0;
    std::uint64_t sessionId = 0;
    std::uint32_t source = 0;
    std::uint32_t destination = 0;
    std::uint32_t sequence = 0;
    std::uint16_t fragmentIndex = 0;
    std::uint16_t fragmentCount = 1;
};

// A decoded envelope. Only the fields of its kind are meaningful.
struct Envelope {
    EnvelopeHeader header;
    std::vector<std::uint8_t> payload; // kData: 1 to 1168 bytes
    std::uint32_t cumulative = 0;      // kAck
    std::uint32_t mask = 0;            // kAck
    std::uint32_t nonce = 0;           // kPing, kPong
    std::uint32_t sentMs = 0;          // kPing, kPong
    std::uint32_t reason = 0;          // kClose
};

// The bytes of `envelope`. The caller keeps to the limits decodeEnvelope() checks; the encoder
// writes the kFragment flag from fragmentCount itself.
std::vector<std::uint8_t> encodeEnvelope(const Envelope &envelope);

// The envelope in `data`, or nothing when it breaks any rule of PROTOCOL.md: a wrong magic, an
// unknown version or kind, an unknown flag bit, a kFragment flag that disagrees with the count,
// a count of 0 or over 64, an index at or past the count, a count other than 1 on a kind other
// than kData, an empty or oversized payload, or a size other than the kind's.
std::optional<Envelope> decodeEnvelope(const std::uint8_t *data, std::size_t bytes);

// The header alone, as the relay reads it: nothing when the datagram is shorter than 32 bytes or
// its magic or version is not this one.
std::optional<EnvelopeHeader> peekEnvelopeHeader(const std::uint8_t *data, std::size_t bytes);

} // namespace sfcop::net
