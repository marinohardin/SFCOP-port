// IDirectPlay4A: the LAN protocol's packet handling and timers (README.md, "Wire format").
// Everything here runs with the object's lock held.
#include "hle/dplay/DirectPlay.h"

#include "hle/dplay/DirectPlayLock.h"
#include "hle/dplay/Layout.h"

#include <algorithm>

namespace sfcop::hle::dplay {

using platform::Result;

namespace {

constexpr std::size_t kMaxDatagramsPerPump = 256;
constexpr std::size_t kMaxEarly = 1024;           // out-of-order channel messages kept
constexpr std::size_t kMaxRetransmitsPerPeer = 64; // per timers() pass
constexpr std::uint32_t kLoopback = 0x7F000001;

bool isLoopback(std::uint32_t address) { return (address >> 24) == 127; }

// How the host names a peer's address to other peers: a loopback address is "the address you
// reach me at" (0), since that peer is on the host's machine.
std::uint32_t advertised(std::uint32_t address) { return isLoopback(address) ? 0 : address; }

} // namespace

void DirectPlay::pump() {
    for (platform::SocketId socket : Sockets()) {
        for (std::size_t n = 0; n < kMaxDatagramsPerPump; ++n) {
            Result<platform::Received> got =
                env_.transport->ReceiveFrom(socket, rx_.data(), rx_.size());
            if (!got.Ok()) {
                break;
            }
            handleDatagram(rx_.data(), got.Value().bytes, got.Value().from);
            if (!game_.Valid()) {
                return; // the session was closed while handling it
            }
        }
    }
    timers();
}

void DirectPlay::handleDatagram(const std::uint8_t *data, std::size_t bytes,
                                platform::Endpoint from) {
    std::optional<wire::Packet> packet = wire::decodePacket(data, bytes);
    if (!packet) {
        return;
    }
    const wire::Packet &p = *packet;
    switch (p.type) {
    case wire::PacketType::kEnumRequest:
        handleEnumRequest(p, from);
        return;
    case wire::PacketType::kEnumReply:
        handleEnumReply(p, from);
        return;
    case wire::PacketType::kJoinRequest:
        handleJoinRequest(p, from);
        return;
    case wire::PacketType::kJoinReject:
        if (state_ == State::kJoining && !joinAnswered_ && p.id == joinInstance_ &&
            from == joinHost_) {
            joinAnswered_ = true;
            joinResult_ = p.result != kDpOk ? p.result : kDpErrGeneric;
        }
        return;
    default:
        break;
    }

    // The host's first channel message to a joiner is the join accept, sequence 1; it is the
    // only peer packet a joiner takes before it knows the session.
    if (state_ == State::kJoining && !joinAnswered_) {
        if (p.type != wire::PacketType::kReliable || p.seq != 1 || from != joinHost_ ||
            !peers_.empty()) {
            return;
        }
        std::optional<wire::Inner> inner = wire::decodeInner(p.body);
        if (!inner || inner->type != wire::InnerType::kJoinAccept) {
            return;
        }
        key_ = p.sessionKey;
        Peer &host = addPeer(p.sender, from);
        handleReliable(host, p);
        return;
    }

    if (state_ != State::kInSession || p.sessionKey != key_) {
        return;
    }
    Peer *peer = peerOf(p.sender);
    if (peer == nullptr) {
        return;
    }
    peer->endpoint = from;
    peer->lastHeard = now();
    switch (p.type) {
    case wire::PacketType::kReliable:
        handleReliable(*peer, p);
        break;
    case wire::PacketType::kAck:
        while (!peer->unacked.empty() && peer->unacked.front().seq <= p.seq) {
            peer->unacked.pop_front();
        }
        break;
    case wire::PacketType::kUnreliable:
        deliverData(p.from, p.to, p.body.data(), p.body.size());
        break;
    case wire::PacketType::kLeave:
        dropPeer(p.sender);
        break;
    default: // keepalive: being heard from is all it says
        break;
    }
}

void DirectPlay::handleEnumRequest(const wire::Packet &p, platform::Endpoint from) {
    if (state_ != State::kInSession || !isHost_) {
        return;
    }
    if (!p.id.IsNull() && p.id != desc_.application) {
        return;
    }
    sendTo(from, wire::encodeEnumReply(p.requestId, currentDesc()));
}

void DirectPlay::handleEnumReply(const wire::Packet &p, platform::Endpoint from) {
    if (!enumerating_ || p.requestId != enumRequest_) {
        return;
    }
    if (!enumApplication_.IsNull() && p.desc.application != enumApplication_) {
        return;
    }
    auto it = found_.find(p.desc.instance);
    if (it != found_.end() && !isLoopback(it->second.host.address) && isLoopback(from.address)) {
        it->second.desc = p.desc; // the same host, also heard through loopback: keep its address
        return;
    }
    if (it == found_.end()) {
        if (found_.size() >= kMaxFound) {
            auto oldest = std::min_element(found_.begin(), found_.end(),
                                           [](const auto &a, const auto &b) {
                                               return a.second.order < b.second.order;
                                           });
            found_.erase(oldest);
            ++counters_.droppedSessions;
        }
        found_[p.desc.instance] = Found{p.desc, from, ++foundOrder_};
    } else {
        it->second.desc = p.desc;
        it->second.host = from;
    }
}

void DirectPlay::handleJoinRequest(const wire::Packet &p, platform::Endpoint from) {
    if (state_ != State::kInSession || !isHost_ || p.id != desc_.instance) {
        return;
    }
    for (const auto &entry : peers_) {
        if (entry.second.endpoint == from) {
            return; // admitted already; the accept is on its way
        }
    }
    HResult reject = kDpOk;
    if (desc_.flags & (kSessionJoinDisabled | kSessionNewPlayersDisabled)) {
        reject = kDpErrNoNewPlayers;
    } else if (desc_.maxPlayers != 0 && playerCount() >= desc_.maxPlayers) {
        reject = kDpErrNoNewPlayers;
    } else if (desc_.password && !desc_.password->empty() && p.password != desc_.password) {
        reject = kDpErrInvalidPassword;
    }
    std::uint32_t id = nextPeer_;
    std::vector<wire::PeerInfo> others;
    for (const auto &entry : peers_) {
        others.push_back(wire::PeerInfo{entry.first, advertised(entry.second.endpoint.address),
                                        entry.second.endpoint.port});
    }
    std::vector<wire::Entity> all;
    for (const auto &entry : entities_) {
        all.push_back(entry.second);
    }
    std::vector<std::uint8_t> accept =
        wire::innerJoinAccept(id, id + 1, currentDesc(), others, all);
    if (reject == kDpOk && accept.size() + 20 > wire::kMaxDatagram) {
        reject = kDpErrCantAddPlayer;
    }
    if (reject != kDpOk) {
        sendTo(from, wire::encodeJoinReject(p.id, reject));
        return;
    }
    ++nextPeer_;
    Peer &joiner = addPeer(id, from);
    sendReliable(joiner, accept);
    std::vector<std::uint8_t> added =
        wire::innerPeerAdd(wire::PeerInfo{id, advertised(from.address), from.port});
    for (auto &entry : peers_) {
        if (entry.first != id) {
            sendReliable(entry.second, added);
        }
    }
}

void DirectPlay::handleReliable(Peer &peer, const wire::Packet &p) {
    if (p.seq == peer.expected) {
        ++peer.expected;
        std::uint32_t id = peer.id;
        if (std::optional<wire::Inner> m = wire::decodeInner(p.body)) {
            handleInner(peer, *m);
        }
        while (Peer *still = peerOf(id)) {
            auto next = still->early.find(still->expected);
            if (next == still->early.end()) {
                break;
            }
            std::vector<std::uint8_t> body = std::move(next->second);
            still->early.erase(next);
            ++still->expected;
            if (std::optional<wire::Inner> m = wire::decodeInner(body)) {
                handleInner(*still, *m);
            }
        }
        if (Peer *still = peerOf(id)) {
            sendTo(still->endpoint, wire::encodeAck(key_, self_, still->expected - 1));
        }
        return;
    }
    if (p.seq > peer.expected && peer.early.size() < kMaxEarly) {
        peer.early.emplace(p.seq, p.body);
    }
    sendTo(peer.endpoint, wire::encodeAck(key_, self_, peer.expected - 1));
}

void DirectPlay::handleInner(Peer &peer, const wire::Inner &m) {
    if (m.type == wire::InnerType::kJoinAccept) {
        if (state_ == State::kJoining && !joinAnswered_) {
            handleJoinAccept(peer, m);
        }
        return;
    }
    if (state_ != State::kInSession) {
        return;
    }
    switch (m.type) {
    case wire::InnerType::kPeerAdd: {
        if (m.peer.id == self_ || peers_.count(m.peer.id) != 0) {
            return;
        }
        std::uint32_t address = m.peer.address != 0 ? m.peer.address : peer.endpoint.address;
        Peer &added = addPeer(m.peer.id, platform::Endpoint{address, m.peer.port});
        nextPeer_ = std::max(nextPeer_, m.peer.id + 1);
        for (const wire::Entity &e : ownEntities()) {
            sendReliable(added, wire::innerEntityAdd(e));
        }
        return;
    }
    case wire::InnerType::kPeerRemove:
        if (m.id != self_ && m.id != peer.id) {
            dropPeer(m.id);
        }
        return;
    case wire::InnerType::kEntityAdd: {
        const wire::Entity &e = m.entities.front();
        auto existing = entities_.find(e.id);
        if (existing != entities_.end()) {
            if (existing->second.owner != e.owner) {
                ++counters_.rejectedChanges; // an id already taken by another owner
            }
            return;
        }
        // A peer announces its own players and groups, with ids from its own range.
        if (e.owner != peer.id || (e.id >> 16) != (e.owner & 0xFFFF)) {
            ++counters_.rejectedChanges;
            return;
        }
        std::size_t owned = static_cast<std::size_t>(std::count_if(
            entities_.begin(), entities_.end(),
            [&](const auto &entry) { return entry.second.owner == peer.id; }));
        if (owned >= kMaxEntitiesPerPeer) {
            ++counters_.rejectedEntities;
            return;
        }
        addEntity(e, false);
        return;
    }
    case wire::InnerType::kEntityRemove: {
        auto it = entities_.find(m.id);
        if (it == entities_.end() || !ownsChange(peer, m.id)) {
            return;
        }
        removeEntity(m.id, false);
        return;
    }
    case wire::InnerType::kSetData: {
        auto it = entities_.find(m.id);
        if (it == entities_.end()) {
            return;
        }
        if (!ownsChange(peer, m.id)) {
            return;
        }
        it->second.data = m.data;
        std::uint32_t type = it->second.group ? kPlayerTypeGroup : kPlayerTypePlayer;
        deliverSystem(layoutSetPlayerOrGroupData(type, m.id, m.data), 0);
        return;
    }
    case wire::InnerType::kSetName: {
        auto it = entities_.find(m.id);
        if (it == entities_.end()) {
            return;
        }
        if (!ownsChange(peer, m.id)) {
            return;
        }
        it->second.name = m.name;
        std::uint32_t type = it->second.group ? kPlayerTypeGroup : kPlayerTypePlayer;
        deliverSystem(layoutSetPlayerOrGroupName(type, m.id, m.name), 0);
        return;
    }
    case wire::InnerType::kMemberAdd:
    case wire::InnerType::kMemberRemove: {
        auto it = entities_.find(m.group);
        if (it == entities_.end() || !it->second.group) {
            return;
        }
        // The group's owner or the player's may change the membership, as Add/DeletePlayer
        // ToGroup require here.
        auto who = entities_.find(m.player);
        bool adds = m.type == wire::InnerType::kMemberAdd;
        if (adds && (who == entities_.end() || who->second.group)) {
            return;
        }
        if (it->second.owner != peer.id && (who == entities_.end() || who->second.owner != peer.id)) {
            ++counters_.rejectedChanges;
            return;
        }
        std::vector<Dpid> &members = it->second.members;
        auto at = std::find(members.begin(), members.end(), m.player);
        if (adds) {
            if (at != members.end()) {
                return;
            }
            members.push_back(m.player);
            deliverSystem(layoutGroupMembership(kSysAddPlayerToGroup, m.group, m.player), 0);
        } else {
            if (at == members.end()) {
                return;
            }
            members.erase(at);
            deliverSystem(layoutGroupMembership(kSysDeletePlayerFromGroup, m.group, m.player),
                          0);
        }
        return;
    }
    case wire::InnerType::kSessionDesc: {
        if (peer.id != host_) {
            return;
        }
        Guid instance = desc_.instance;
        desc_ = m.desc;
        desc_.instance = instance;
        deliverSystem(layoutSetSessionDesc(currentDesc()), 0);
        return;
    }
    case wire::InnerType::kData:
        deliverData(m.from, m.to, m.data.data(), m.data.size());
        return;
    case wire::InnerType::kJoinAccept:
        return;
    }
}

// A change to an entity is applied only when the machine that sent it owns the entity. The
// count of refusals is what a test, or a diagnosis, reads.
bool DirectPlay::ownsChange(const Peer &sender, Dpid entity) {
    auto it = entities_.find(entity);
    if (it != entities_.end() && it->second.owner == sender.id) {
        return true;
    }
    ++counters_.rejectedChanges;
    return false;
}

void DirectPlay::handleJoinAccept(Peer &host, const wire::Inner &m) {
    self_ = m.yourPeer;
    host_ = host.id;
    nextPeer_ = m.nextPeer;
    desc_ = m.desc;
    desc_.instance = joinInstance_;
    isHost_ = false;
    nextLocal_ = 1;
    for (const wire::PeerInfo &p : m.peers) {
        if (p.id == self_ || p.id == host.id || peers_.count(p.id) != 0) {
            continue;
        }
        std::uint32_t address = p.address != 0 ? p.address : joinHost_.address;
        addPeer(p.id, platform::Endpoint{address, p.port});
    }
    for (const wire::Entity &e : m.entities) {
        entities_[e.id] = e;
    }
    state_ = State::kInSession;
    joinAnswered_ = true;
    joinResult_ = kDpOk;
}

void DirectPlay::timers() {
    std::uint32_t t = now();
    if (enumerating_ && t - lastEnumSent_ >= config_.resendMs) {
        sendEnumRequests();
    }
    if (state_ == State::kJoining && !joinAnswered_ && t - lastJoinSent_ >= config_.resendMs) {
        sendTo(joinHost_, wire::encodeJoinRequest(joinInstance_, joinPassword_));
        lastJoinSent_ = t;
    }
    if (state_ != State::kInSession) {
        return;
    }
    std::vector<std::uint32_t> gone;
    for (auto &entry : peers_) {
        Peer &peer = entry.second;
        if (t - peer.lastHeard >= config_.peerTimeoutMs) {
            gone.push_back(peer.id);
            continue;
        }
        std::size_t resent = 0;
        for (Unacked &u : peer.unacked) {
            if (resent == kMaxRetransmitsPerPeer) {
                break;
            }
            if (t - u.lastSent >= config_.retransmitMs) {
                sendTo(peer.endpoint, u.datagram);
                u.lastSent = t;
                peer.lastSent = t;
                ++resent;
            }
        }
        if (t - peer.lastSent >= config_.keepaliveMs) {
            sendTo(peer.endpoint, wire::encodeKeepalive(key_, self_));
            peer.lastSent = t;
        }
    }
    for (std::uint32_t id : gone) {
        dropPeer(id);
    }
}

void DirectPlay::dropPeer(std::uint32_t id) {
    auto it = peers_.find(id);
    if (it == peers_.end()) {
        return;
    }
    peers_.erase(it);
    std::vector<Dpid> owned;
    for (const auto &entry : entities_) {
        if (entry.second.owner == id) {
            owned.push_back(entry.first);
        }
    }
    for (Dpid e : owned) {
        removeEntity(e, false);
    }
    if (isHost_) {
        broadcastReliable(wire::innerPeerRemove(id));
    }
    if (id != host_) {
        return;
    }
    if (desc_.flags & kSessionMigrateHost) {
        std::uint32_t next = self_;
        for (const auto &entry : peers_) {
            next = std::min(next, entry.first);
        }
        host_ = next;
        if (next == self_) {
            becomeHost();
        }
    } else {
        host_ = 0;
        if (state_ == State::kInSession) {
            state_ = State::kLost;
        }
        deliverSystem(layoutTypeOnly(kSysSessionLost), 0);
    }
}

void DirectPlay::becomeHost() {
    isHost_ = true;
    host_ = self_;
    std::uint32_t next = std::max(nextPeer_, self_ + 1);
    for (const auto &entry : peers_) {
        next = std::max(next, entry.first + 1);
    }
    nextPeer_ = next;
    // Without the enumeration port the session goes on, and only new machines cannot find it.
    (void)openSockets(true);
    deliverSystem(layoutTypeOnly(kSysHost), 0);
}

void DirectPlay::sendTo(platform::Endpoint to, const std::vector<std::uint8_t> &datagram) {
    if (!game_.Valid()) {
        return;
    }
    (void)env_.transport->SendTo(game_, datagram.data(), datagram.size(), to);
}

void DirectPlay::sendReliable(Peer &peer, const std::vector<std::uint8_t> &inner,
                              std::uint32_t payloadBytes) {
    Unacked u;
    u.seq = peer.nextSeq++;
    u.datagram = wire::encodeReliable(key_, self_, u.seq, inner);
    u.payloadBytes = payloadBytes;
    u.lastSent = now();
    sendTo(peer.endpoint, u.datagram);
    peer.lastSent = u.lastSent;
    if (peer.unacked.size() >= kMaxUnacked) {
        peer.unacked.pop_front(); // a peer that acknowledges nothing is dropped by its timeout
        ++counters_.droppedUnacked;
    }
    peer.unacked.push_back(std::move(u));
}

void DirectPlay::broadcastReliable(const std::vector<std::uint8_t> &inner) {
    for (auto &entry : peers_) {
        sendReliable(entry.second, inner);
    }
}

void DirectPlay::sendEnumRequests() {
    std::vector<std::uint8_t> request = wire::encodeEnumRequest(enumRequest_, enumApplication_);
    if (haveTarget_) {
        sendTo(target_, request);
    } else {
        sendTo(platform::Endpoint{config_.broadcastAddress, config_.enumPort}, request);
        if (config_.searchLoopback) {
            sendTo(platform::Endpoint{kLoopback, config_.enumPort}, request);
        }
    }
    lastEnumSent_ = now();
}

DirectPlay::Peer &DirectPlay::addPeer(std::uint32_t id, platform::Endpoint endpoint) {
    Peer &p = peers_[id];
    p.id = id;
    p.endpoint = endpoint;
    p.lastHeard = now();
    p.lastSent = p.lastHeard;
    return p;
}

DirectPlay::Peer *DirectPlay::peerOf(std::uint32_t id) {
    auto it = peers_.find(id);
    return it == peers_.end() ? nullptr : &it->second;
}

} // namespace sfcop::hle::dplay
