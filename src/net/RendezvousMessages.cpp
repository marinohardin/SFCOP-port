#include "net/RendezvousMessages.h"

#include "net/Bytes.h"

namespace sfcop::net {
namespace {

constexpr std::size_t kCandidateBytes = 8;

void writeCandidate(ByteWriter &writer, const Candidate &candidate) {
    writer.U32(candidate.address);
    writer.U16(candidate.port);
    writer.U8(static_cast<std::uint8_t>(candidate.kind));
    writer.U8(0);
}

bool readCandidate(ByteReader &reader, Candidate &out) {
    out.address = reader.U32();
    out.port = reader.U16();
    std::uint8_t kind = reader.U8();
    reader.Skip(1);
    if (kind != 1 && kind != 2) {
        return false;
    }
    out.kind = static_cast<CandidateKind>(kind);
    return reader.Ok();
}

bool readCandidates(ByteReader &reader, std::size_t count, std::vector<Candidate> &out) {
    if (reader.Remaining() != count * kCandidateBytes) {
        return false;
    }
    out.resize(count);
    for (Candidate &candidate : out) {
        if (!readCandidate(reader, candidate)) {
            return false;
        }
    }
    return true;
}

bool readResult(ByteReader &reader, RvResult &out) {
    std::uint32_t code = reader.U32();
    if (code > static_cast<std::uint32_t>(RvResult::kFull)) {
        return false;
    }
    out = static_cast<RvResult>(code);
    return true;
}

} // namespace

TicketFields readTicket(const Ticket &ticket) {
    ByteReader reader(ticket.data(), ticket.size());
    TicketFields fields;
    fields.sessionId = reader.U64();
    fields.peerId = reader.U32();
    fields.expiresAt = reader.U32();
    fields.role = reader.U8();
    reader.Skip(3);
    reader.Bytes(fields.mac.data(), fields.mac.size());
    return fields;
}

Ticket writeTicket(const TicketFields &fields) {
    std::vector<std::uint8_t> bytes;
    ByteWriter writer(bytes);
    writer.U64(fields.sessionId);
    writer.U32(fields.peerId);
    writer.U32(fields.expiresAt);
    writer.U8(fields.role);
    writer.Zeros(3);
    writer.Bytes(fields.mac.data(), fields.mac.size());
    Ticket ticket{};
    for (std::size_t i = 0; i < ticket.size(); ++i) {
        ticket[i] = bytes[i];
    }
    return ticket;
}

std::vector<std::uint8_t> encodeRendezvous(const RvMessage &message) {
    std::vector<std::uint8_t> out;
    ByteWriter writer(out);
    writer.U32(kRendezvousMagic);
    writer.U8(kRendezvousVersion);
    writer.U8(static_cast<std::uint8_t>(message.type));
    writer.U16(0);
    writer.U32(message.transaction);
    switch (message.type) {
    case RvType::kBind:
        writer.Bytes(message.ticket.data(), message.ticket.size());
        writer.U8(static_cast<std::uint8_t>(message.candidates.size()));
        writer.U8(0);
        for (const Candidate &candidate : message.candidates) {
            writeCandidate(writer, candidate);
        }
        break;
    case RvType::kBindReply:
        writer.U32(static_cast<std::uint32_t>(message.result));
        writeCandidate(writer, message.reflexive);
        writer.U16(message.refreshSeconds);
        writer.U16(0);
        break;
    case RvType::kConnect:
        writer.U64(message.sessionId);
        writer.U32(message.source);
        writer.U32(message.target);
        break;
    case RvType::kPeerCandidates:
        writer.U64(message.sessionId);
        writer.U32(message.peer);
        writer.U32(message.punchId);
        writer.U8(static_cast<std::uint8_t>(message.candidates.size()));
        writer.Zeros(3);
        for (const Candidate &candidate : message.candidates) {
            writeCandidate(writer, candidate);
        }
        break;
    case RvType::kProbe:
    case RvType::kProbeReply:
        writer.U64(message.sessionId);
        writer.U32(message.source);
        writer.U32(message.target);
        writer.U32(message.punchId);
        break;
    case RvType::kUnbind:
        writer.U64(message.sessionId);
        writer.U32(message.peer);
        break;
    case RvType::kError:
        writer.U32(static_cast<std::uint32_t>(message.result));
        break;
    }
    return out;
}

std::optional<RvMessage> decodeRendezvous(const std::uint8_t *data, std::size_t bytes) {
    ByteReader reader(data, bytes);
    std::uint32_t magic = reader.U32();
    std::uint8_t version = reader.U8();
    std::uint8_t type = reader.U8();
    reader.Skip(2);
    RvMessage message;
    message.transaction = reader.U32();
    if (!reader.Ok() || magic != kRendezvousMagic || version != kRendezvousVersion || type < 1 ||
        type > 8) {
        return std::nullopt;
    }
    message.type = static_cast<RvType>(type);
    bool ok = true;
    switch (message.type) {
    case RvType::kBind: {
        reader.Bytes(message.ticket.data(), message.ticket.size());
        std::size_t count = reader.U8();
        reader.Skip(1);
        ok = reader.Ok() && count <= kMaxBindCandidates &&
             readCandidates(reader, count, message.candidates);
        break;
    }
    case RvType::kBindReply:
        ok = readResult(reader, message.result) && readCandidate(reader, message.reflexive);
        message.refreshSeconds = reader.U16();
        reader.Skip(2);
        break;
    case RvType::kConnect:
        message.sessionId = reader.U64();
        message.source = reader.U32();
        message.target = reader.U32();
        break;
    case RvType::kPeerCandidates: {
        message.sessionId = reader.U64();
        message.peer = reader.U32();
        message.punchId = reader.U32();
        std::size_t count = reader.U8();
        reader.Skip(3);
        ok = reader.Ok() && count >= 1 && count <= kMaxPeerCandidates &&
             readCandidates(reader, count, message.candidates);
        break;
    }
    case RvType::kProbe:
    case RvType::kProbeReply:
        message.sessionId = reader.U64();
        message.source = reader.U32();
        message.target = reader.U32();
        message.punchId = reader.U32();
        break;
    case RvType::kUnbind:
        message.sessionId = reader.U64();
        message.peer = reader.U32();
        break;
    case RvType::kError:
        ok = readResult(reader, message.result);
        break;
    }
    if (!ok || !reader.Done()) {
        return std::nullopt;
    }
    return message;
}

std::optional<std::uint8_t> rendezvousVersion(const std::uint8_t *data, std::size_t bytes) {
    ByteReader reader(data, bytes);
    std::uint32_t magic = reader.U32();
    std::uint8_t version = reader.U8();
    if (!reader.Ok() || magic != kRendezvousMagic) {
        return std::nullopt;
    }
    return version;
}

} // namespace sfcop::net
