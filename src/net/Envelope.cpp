#include "net/Envelope.h"

#include "net/Bytes.h"

namespace sfcop::net {
namespace {

constexpr std::uint8_t kKnownFlags = kEnvelopeReliable | kEnvelopeFragment;

// The exact size of each kind but kData (which is the header and 1 to 1168 bytes).
std::size_t fixedSize(EnvelopeKind kind) {
    switch (kind) {
    case EnvelopeKind::kAck:
    case EnvelopeKind::kPing:
    case EnvelopeKind::kPong:
        return 40;
    case EnvelopeKind::kClose:
        return 36;
    case EnvelopeKind::kData:
        break;
    }
    return 0;
}

// The header, with the kind byte as read in `rawKind` (EnvelopeHeader::kind holds it only when it
// is a known kind).
std::optional<EnvelopeHeader> readHeader(ByteReader &reader, std::uint8_t &rawKind) {
    std::uint32_t magic = reader.U32();
    std::uint8_t version = reader.U8();
    rawKind = reader.U8();
    EnvelopeHeader header;
    header.flags = reader.U8();
    reader.Skip(1); // reserved; a receiver ignores it
    header.sessionId = reader.U64();
    header.source = reader.U32();
    header.destination = reader.U32();
    header.sequence = reader.U32();
    header.fragmentIndex = reader.U16();
    header.fragmentCount = reader.U16();
    if (!reader.Ok() || magic != kEnvelopeMagic || version != kEnvelopeVersion) {
        return std::nullopt;
    }
    if (rawKind >= 1 && rawKind <= 5) {
        header.kind = static_cast<EnvelopeKind>(rawKind);
    }
    return header;
}

} // namespace

std::vector<std::uint8_t> encodeEnvelope(const Envelope &envelope) {
    const EnvelopeHeader &header = envelope.header;
    std::vector<std::uint8_t> out;
    out.reserve(kEnvelopeHeaderBytes + envelope.payload.size());
    ByteWriter writer(out);
    writer.U32(kEnvelopeMagic);
    writer.U8(kEnvelopeVersion);
    writer.U8(static_cast<std::uint8_t>(header.kind));
    std::uint8_t flags = header.flags & kEnvelopeReliable;
    if (header.fragmentCount > 1) {
        flags |= kEnvelopeFragment;
    }
    writer.U8(flags);
    writer.U8(0);
    writer.U64(header.sessionId);
    writer.U32(header.source);
    writer.U32(header.destination);
    writer.U32(header.sequence);
    writer.U16(header.fragmentIndex);
    writer.U16(header.fragmentCount);
    switch (header.kind) {
    case EnvelopeKind::kData:
        writer.Bytes(envelope.payload.data(), envelope.payload.size());
        break;
    case EnvelopeKind::kAck:
        writer.U32(envelope.cumulative);
        writer.U32(envelope.mask);
        break;
    case EnvelopeKind::kPing:
    case EnvelopeKind::kPong:
        writer.U32(envelope.nonce);
        writer.U32(envelope.sentMs);
        break;
    case EnvelopeKind::kClose:
        writer.U32(envelope.reason);
        break;
    }
    return out;
}

std::optional<Envelope> decodeEnvelope(const std::uint8_t *data, std::size_t bytes) {
    if (bytes > kMaxEnvelopeBytes) {
        return std::nullopt;
    }
    ByteReader reader(data, bytes);
    std::uint8_t rawKind = 0;
    std::optional<EnvelopeHeader> header = readHeader(reader, rawKind);
    if (!header || rawKind < 1 || rawKind > 5 || (header->flags & ~kKnownFlags) != 0) {
        return std::nullopt;
    }
    const bool fragmented = (header->flags & kEnvelopeFragment) != 0;
    if (header->fragmentCount < 1 || header->fragmentCount > kMaxFragments ||
        header->fragmentIndex >= header->fragmentCount ||
        fragmented != (header->fragmentCount > 1)) {
        return std::nullopt;
    }
    Envelope envelope;
    envelope.header = *header;
    if (header->kind == EnvelopeKind::kData) {
        std::size_t payload = reader.Remaining();
        if (payload < 1 || payload > kMaxFragmentPayload) {
            return std::nullopt;
        }
        envelope.payload.assign(reader.Here(), reader.Here() + payload);
        return envelope;
    }
    if (header->fragmentCount != 1 || bytes != fixedSize(header->kind)) {
        return std::nullopt;
    }
    switch (header->kind) {
    case EnvelopeKind::kAck:
        envelope.cumulative = reader.U32();
        envelope.mask = reader.U32();
        break;
    case EnvelopeKind::kPing:
    case EnvelopeKind::kPong:
        envelope.nonce = reader.U32();
        envelope.sentMs = reader.U32();
        break;
    case EnvelopeKind::kClose:
        envelope.reason = reader.U32();
        break;
    case EnvelopeKind::kData:
        break;
    }
    if (!reader.Done()) {
        return std::nullopt;
    }
    return envelope;
}

std::optional<EnvelopeHeader> peekEnvelopeHeader(const std::uint8_t *data, std::size_t bytes) {
    ByteReader reader(data, bytes);
    std::uint8_t rawKind = 0;
    return readHeader(reader, rawKind);
}

} // namespace sfcop::net
