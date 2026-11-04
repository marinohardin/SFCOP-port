#include "hle/dplay/Wire.h"

#include "hle/dplay/Bytes.h"

namespace sfcop::hle::dplay::wire {

namespace {

// Peer packets carry 16 bytes of header: magic, version, type, two reserved bytes, the session
// key and the sender's peer id. Enumeration and join packets carry the first 8.
constexpr std::size_t kMaxPeers = 1024;
constexpr std::size_t kMaxEntities = 4096;
constexpr std::size_t kMaxMembers = 4096;

ByteWriter header(PacketType type) {
    ByteWriter w;
    w.U32(kMagic);
    w.U8(kVersion);
    w.U8(static_cast<std::uint8_t>(type));
    w.U16(0);
    return w;
}

ByteWriter peerHeader(PacketType type, std::uint32_t key, std::uint32_t sender) {
    ByteWriter w = header(type);
    w.U32(key);
    w.U32(sender);
    return w;
}

void putDesc(ByteWriter &w, const SessionDesc &d, bool withPassword) {
    std::uint32_t flags = d.flags;
    if (!withPassword && d.password && !d.password->empty()) {
        flags |= kSessionPasswordRequired;
    }
    w.U32(flags);
    w.Id(d.instance);
    w.Id(d.application);
    w.U32(d.maxPlayers);
    w.U32(d.currentPlayers);
    for (std::uint32_t u : d.user) {
        w.U32(u);
    }
    w.Str(d.name);
    w.Str(withPassword ? d.password : std::nullopt);
}

SessionDesc getDesc(ByteReader &r) {
    SessionDesc d;
    d.flags = r.U32();
    d.instance = r.Id();
    d.application = r.Id();
    d.maxPlayers = r.U32();
    d.currentPlayers = r.U32();
    for (std::uint32_t &u : d.user) {
        u = r.U32();
    }
    d.name = r.Str();
    d.password = r.Str();
    return d;
}

void putName(ByteWriter &w, const Name &n) {
    w.Str(n.shortName);
    w.Str(n.longName);
}

Name getName(ByteReader &r) {
    Name n;
    n.shortName = r.Str();
    n.longName = r.Str();
    return n;
}

void putEntity(ByteWriter &w, const Entity &e) {
    w.U32(e.id);
    w.U8(e.group ? 1 : 0);
    w.U32(e.flags);
    w.U32(e.owner);
    putName(w, e.name);
    w.Bytes(e.data);
    w.U16(static_cast<std::uint16_t>(e.members.size()));
    for (Dpid m : e.members) {
        w.U32(m);
    }
}

bool getEntity(ByteReader &r, Entity &e) {
    e.id = r.U32();
    std::uint8_t group = r.U8();
    e.flags = r.U32();
    e.owner = r.U32();
    e.name = getName(r);
    e.data = r.Bytes();
    std::uint16_t members = r.U16();
    if (group > 1 || members > kMaxMembers) {
        return false;
    }
    e.group = group == 1;
    for (std::uint16_t i = 0; i < members && r.Ok(); ++i) {
        e.members.push_back(r.U32());
    }
    return r.Ok();
}

void putPeer(ByteWriter &w, const PeerInfo &p) {
    w.U32(p.id);
    w.U32(p.address);
    w.U16(p.port);
}

PeerInfo getPeer(ByteReader &r) {
    PeerInfo p;
    p.id = r.U32();
    p.address = r.U32();
    p.port = r.U16();
    return p;
}

ByteWriter inner(InnerType type) {
    ByteWriter w;
    w.U8(static_cast<std::uint8_t>(type));
    return w;
}

} // namespace

std::vector<std::uint8_t> encodeEnumRequest(std::uint32_t requestId, const Guid &application) {
    ByteWriter w = header(PacketType::kEnumRequest);
    w.U32(requestId);
    w.Id(application);
    return w.Take();
}

std::vector<std::uint8_t> encodeEnumReply(std::uint32_t requestId, const SessionDesc &desc) {
    ByteWriter w = header(PacketType::kEnumReply);
    w.U32(requestId);
    putDesc(w, desc, false);
    return w.Take();
}

std::vector<std::uint8_t> encodeJoinRequest(const Guid &instance,
                                            const std::optional<std::string> &password) {
    ByteWriter w = header(PacketType::kJoinRequest);
    w.Id(instance);
    w.Str(password);
    return w.Take();
}

std::vector<std::uint8_t> encodeJoinReject(const Guid &instance, HResult result) {
    ByteWriter w = header(PacketType::kJoinReject);
    w.Id(instance);
    w.U32(result);
    return w.Take();
}

std::vector<std::uint8_t> encodeReliable(std::uint32_t key, std::uint32_t sender,
                                         std::uint32_t seq,
                                         const std::vector<std::uint8_t> &innerBody) {
    ByteWriter w = peerHeader(PacketType::kReliable, key, sender);
    w.U32(seq);
    w.Raw(innerBody);
    return w.Take();
}

std::vector<std::uint8_t> encodeAck(std::uint32_t key, std::uint32_t sender,
                                    std::uint32_t cumulative) {
    ByteWriter w = peerHeader(PacketType::kAck, key, sender);
    w.U32(cumulative);
    return w.Take();
}

std::vector<std::uint8_t> encodeUnreliable(std::uint32_t key, std::uint32_t sender, Dpid from,
                                           Dpid to, const std::uint8_t *payload,
                                           std::size_t bytes) {
    ByteWriter w = peerHeader(PacketType::kUnreliable, key, sender);
    w.U32(from);
    w.U32(to);
    w.Raw(payload, bytes);
    return w.Take();
}

std::vector<std::uint8_t> encodeKeepalive(std::uint32_t key, std::uint32_t sender) {
    return peerHeader(PacketType::kKeepalive, key, sender).Take();
}

std::vector<std::uint8_t> encodeLeave(std::uint32_t key, std::uint32_t sender) {
    return peerHeader(PacketType::kLeave, key, sender).Take();
}

std::optional<Packet> decodePacket(const std::uint8_t *data, std::size_t bytes) {
    ByteReader r(data, bytes);
    if (r.U32() != kMagic || r.U8() != kVersion) {
        return std::nullopt;
    }
    Packet p;
    std::uint8_t type = r.U8();
    r.U16();
    if (!r.Ok()) {
        return std::nullopt;
    }
    switch (type) {
    case static_cast<std::uint8_t>(PacketType::kEnumRequest):
        p.requestId = r.U32();
        p.id = r.Id();
        break;
    case static_cast<std::uint8_t>(PacketType::kEnumReply):
        p.requestId = r.U32();
        p.desc = getDesc(r);
        break;
    case static_cast<std::uint8_t>(PacketType::kJoinRequest):
        p.id = r.Id();
        p.password = r.Str();
        break;
    case static_cast<std::uint8_t>(PacketType::kJoinReject):
        p.id = r.Id();
        p.result = r.U32();
        break;
    case static_cast<std::uint8_t>(PacketType::kReliable):
    case static_cast<std::uint8_t>(PacketType::kAck):
    case static_cast<std::uint8_t>(PacketType::kUnreliable):
    case static_cast<std::uint8_t>(PacketType::kKeepalive):
    case static_cast<std::uint8_t>(PacketType::kLeave):
        p.sessionKey = r.U32();
        p.sender = r.U32();
        if (type == static_cast<std::uint8_t>(PacketType::kReliable)) {
            p.seq = r.U32();
            p.body = r.Rest();
            if (p.body.empty()) {
                return std::nullopt;
            }
        } else if (type == static_cast<std::uint8_t>(PacketType::kAck)) {
            p.seq = r.U32();
        } else if (type == static_cast<std::uint8_t>(PacketType::kUnreliable)) {
            p.from = r.U32();
            p.to = r.U32();
            p.body = r.Rest();
        }
        break;
    default:
        return std::nullopt;
    }
    if (!r.AtEnd()) {
        return std::nullopt;
    }
    p.type = static_cast<PacketType>(type);
    return p;
}

std::vector<std::uint8_t> innerJoinAccept(std::uint32_t yourPeer, std::uint32_t nextPeer,
                                          const SessionDesc &desc,
                                          const std::vector<PeerInfo> &peers,
                                          const std::vector<Entity> &entities) {
    ByteWriter w = inner(InnerType::kJoinAccept);
    w.U32(yourPeer);
    w.U32(nextPeer);
    putDesc(w, desc, true);
    w.U16(static_cast<std::uint16_t>(peers.size()));
    for (const PeerInfo &p : peers) {
        putPeer(w, p);
    }
    w.U16(static_cast<std::uint16_t>(entities.size()));
    for (const Entity &e : entities) {
        putEntity(w, e);
    }
    return w.Take();
}

std::vector<std::uint8_t> innerPeerAdd(const PeerInfo &peer) {
    ByteWriter w = inner(InnerType::kPeerAdd);
    putPeer(w, peer);
    return w.Take();
}

std::vector<std::uint8_t> innerPeerRemove(std::uint32_t peer) {
    ByteWriter w = inner(InnerType::kPeerRemove);
    w.U32(peer);
    return w.Take();
}

std::vector<std::uint8_t> innerEntityAdd(const Entity &entity) {
    ByteWriter w = inner(InnerType::kEntityAdd);
    putEntity(w, entity);
    return w.Take();
}

std::vector<std::uint8_t> innerEntityRemove(Dpid id) {
    ByteWriter w = inner(InnerType::kEntityRemove);
    w.U32(id);
    return w.Take();
}

std::vector<std::uint8_t> innerSetData(Dpid id, const std::vector<std::uint8_t> &data) {
    ByteWriter w = inner(InnerType::kSetData);
    w.U32(id);
    w.Bytes(data);
    return w.Take();
}

std::vector<std::uint8_t> innerSetName(Dpid id, const Name &name) {
    ByteWriter w = inner(InnerType::kSetName);
    w.U32(id);
    putName(w, name);
    return w.Take();
}

std::vector<std::uint8_t> innerMember(bool add, Dpid group, Dpid player) {
    ByteWriter w = inner(add ? InnerType::kMemberAdd : InnerType::kMemberRemove);
    w.U32(group);
    w.U32(player);
    return w.Take();
}

std::vector<std::uint8_t> innerSessionDesc(const SessionDesc &desc) {
    ByteWriter w = inner(InnerType::kSessionDesc);
    putDesc(w, desc, true);
    return w.Take();
}

std::vector<std::uint8_t> innerData(Dpid from, Dpid to, const std::uint8_t *payload,
                                    std::size_t bytes) {
    ByteWriter w = inner(InnerType::kData);
    w.U32(from);
    w.U32(to);
    w.Raw(payload, bytes);
    return w.Take();
}

std::optional<Inner> decodeInner(const std::vector<std::uint8_t> &body) {
    ByteReader r(body);
    Inner m;
    std::uint8_t type = r.U8();
    switch (type) {
    case static_cast<std::uint8_t>(InnerType::kJoinAccept): {
        m.yourPeer = r.U32();
        m.nextPeer = r.U32();
        m.desc = getDesc(r);
        std::uint16_t peers = r.U16();
        if (peers > kMaxPeers) {
            return std::nullopt;
        }
        for (std::uint16_t i = 0; i < peers && r.Ok(); ++i) {
            m.peers.push_back(getPeer(r));
        }
        std::uint16_t entities = r.U16();
        if (entities > kMaxEntities) {
            return std::nullopt;
        }
        for (std::uint16_t i = 0; i < entities && r.Ok(); ++i) {
            Entity e;
            if (!getEntity(r, e)) {
                return std::nullopt;
            }
            m.entities.push_back(std::move(e));
        }
        break;
    }
    case static_cast<std::uint8_t>(InnerType::kPeerAdd):
        m.peer = getPeer(r);
        break;
    case static_cast<std::uint8_t>(InnerType::kPeerRemove):
    case static_cast<std::uint8_t>(InnerType::kEntityRemove):
        m.id = r.U32();
        break;
    case static_cast<std::uint8_t>(InnerType::kEntityAdd): {
        Entity e;
        if (!getEntity(r, e)) {
            return std::nullopt;
        }
        m.entities.push_back(std::move(e));
        break;
    }
    case static_cast<std::uint8_t>(InnerType::kSetData):
        m.id = r.U32();
        m.data = r.Bytes();
        break;
    case static_cast<std::uint8_t>(InnerType::kSetName):
        m.id = r.U32();
        m.name = getName(r);
        break;
    case static_cast<std::uint8_t>(InnerType::kMemberAdd):
    case static_cast<std::uint8_t>(InnerType::kMemberRemove):
        m.group = r.U32();
        m.player = r.U32();
        break;
    case static_cast<std::uint8_t>(InnerType::kSessionDesc):
        m.desc = getDesc(r);
        break;
    case static_cast<std::uint8_t>(InnerType::kData):
        m.from = r.U32();
        m.to = r.U32();
        m.data = r.Rest();
        break;
    default:
        return std::nullopt;
    }
    if (!r.AtEnd()) {
        return std::nullopt;
    }
    m.type = static_cast<InnerType>(type);
    return m;
}

} // namespace sfcop::hle::dplay::wire
