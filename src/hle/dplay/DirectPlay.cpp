// IDirectPlay4A: the methods. The packet handling and timers are in DirectPlayNet.cpp.
#include "hle/dplay/DirectPlay.h"

#include "hle/dplay/Address.h"
#include "hle/dplay/DirectPlayLock.h"
#include "hle/dplay/Layout.h"

#include <algorithm>
#include <cstdio>

namespace sfcop::hle::dplay {

using platform::Result;

namespace {

constexpr std::uint32_t kWaitStepMs = 10;
const char kConnectionName[] = "Internet TCP/IP Connection";
const char kModemConnectionName[] = "Modem Connection For DirectPlay";

std::uint64_t splitmix(std::uint64_t &state) {
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

std::uint32_t keyOf(const Guid &g) {
    std::uint32_t key = 0;
    for (std::size_t i = 0; i < 16; ++i) {
        key ^= static_cast<std::uint32_t>(g.bytes[i]) << (8 * (i % 4));
    }
    return key == 0 ? 1 : key;
}

bool available(const SessionDesc &d) {
    if (d.flags & (kSessionJoinDisabled | kSessionNewPlayersDisabled)) {
        return false;
    }
    return d.maxPlayers == 0 || d.currentPlayers < d.maxPlayers;
}

} // namespace

DirectPlay::DirectPlay(Environment env, Config config)
    : env_(std::move(env)), config_(config), rx_(wire::kMaxDatagram + 1),
      random_(config.seed) {}

DirectPlay::~DirectPlay() {
    stopService_ = true;
    if (service_ && env_.threads != nullptr) {
        // The service thread may be inside Environment::signal, waiting for the game's machine,
        // which the thread running this destructor holds: give the machine up while joining,
        // and wait for as long as it takes. The object is not freed under a running thread.
        platform::Waitable thread = *service_;
        bool joined = false;
        auto join = [&]() {
            Result<platform::WaitResult> waited =
                env_.threads->Wait(&thread, 1, platform::WaitMode::kAny, std::nullopt);
            joined = waited.Ok() && waited.Value().outcome == platform::WaitOutcome::kSignaled;
        };
        if (env_.blocking) {
            env_.blocking(join);
        } else {
            join();
        }
        if (!joined) {
            // Threads could not join it. Its body says when it is done with the object.
            std::fprintf(stderr, "dplay: the service thread could not be joined; waiting for it\n");
            auto spin = [&]() {
                while (serviceStarted_ && !serviceExited_) {
                    (void)env_.clock->Sleep(1);
                }
            };
            if (env_.blocking) {
                env_.blocking(spin);
            } else {
                spin();
            }
        }
        (void)env_.threads->Close(thread);
    }
    Lock lock(*this);
    closeSession();
    if (game_.Valid()) {
        (void)env_.transport->Close(game_);
        game_ = {};
    }
}

std::uint32_t DirectPlay::AddRef() { return ++refs_; }

std::uint32_t DirectPlay::Release() { return --refs_; }

// ---- connection -----------------------------------------------------------------------------

HResult DirectPlay::EnumConnections(const Guid *, const ConnectionCallback &callback,
                                    std::uint32_t flags) {
    if (!callback) {
        return kDpErrInvalidParams;
    }
    if (flags != 0 && (flags & kConnectionDirectPlay) == 0) {
        return kDpOk; // lobby connections only: there are none
    }
    Connection c;
    c.serviceProvider = kSpTcpIp;
    c.address = tcpIpConnection();
    c.name.shortName = kConnectionName;
    c.flags = kConnectionDirectPlay;
    if (!callback(c)) {
        return kDpOk;
    }
    // Listed so that the game finds the entry it expects (DplayDefs.h, kSpModem);
    // InitializeConnection on it answers DPERR_UNAVAILABLE.
    Connection modem;
    modem.serviceProvider = kSpModem;
    modem.address = modemConnection();
    modem.name.shortName = kModemConnectionName;
    modem.flags = kConnectionDirectPlay;
    callback(modem);
    return kDpOk;
}

HResult DirectPlay::InitializeConnection(const std::uint8_t *address, std::size_t bytes,
                                         std::uint32_t flags) {
    Lock lock(*this);
    if (flags != 0) {
        return kDpErrInvalidFlags;
    }
    if (state_ != State::kCreated) {
        return kDpErrAlreadyInitialized;
    }
    std::optional<std::vector<AddressElement>> elements = parseAddress(address, bytes);
    if (!elements) {
        return kDpErrInvalidParams;
    }
    std::optional<Guid> provider = serviceProviderOf(*elements);
    if (!provider) {
        return kDpErrInvalidParams;
    }
    if (*provider != kSpTcpIp) {
        return kDpErrUnavailable;
    }
    TcpIpTarget target = tcpIpTargetOf(*elements);
    haveTarget_ = !target.host.empty();
    if (haveTarget_) {
        std::optional<std::uint32_t> quad = parseDottedQuad(target.host);
        if (!quad) {
            Result<std::vector<std::uint32_t>> resolved = env_.transport->Resolve(target.host);
            if (!resolved.Ok() || resolved.Value().empty()) {
                return kDpErrUnavailable;
            }
            quad = resolved.Value().front();
        }
        target_ = platform::Endpoint{*quad, target.port.value_or(config_.enumPort)};
    }
    state_ = State::kInitialized;
    return kDpOk;
}

// ---- sessions -------------------------------------------------------------------------------

HResult DirectPlay::EnumSessions(const SessionDesc &filter, std::uint32_t timeoutMs,
                                 const SessionCallback &callback, std::uint32_t flags) {
    Lock lock(*this);
    if (state_ == State::kCreated) {
        return kDpErrUninitialized;
    }
    if (flags & kEnumSessionsStopAsync) {
        asyncEnum_ = false;
        enumerating_ = false;
        return kDpOk;
    }
    if (!callback) {
        return kDpErrInvalidParams;
    }
    HResult opened = openSockets(false);
    if (opened != kDpOk) {
        return opened;
    }
    std::uint32_t timeout = timeoutMs != 0 ? timeoutMs : config_.timeoutMs;
    auto startSearch = [&] {
        ++enumRequest_;
        found_.clear();
        enumApplication_ = filter.application;
        enumerating_ = true;
        sendEnumRequests();
    };
    auto selected = [&] {
        std::vector<SessionDesc> list;
        for (const auto &entry : found_) {
            const SessionDesc &d = entry.second.desc;
            if (!filter.application.IsNull() && d.application != filter.application) {
                continue;
            }
            if (!(flags & kEnumSessionsAll) && !available(d)) {
                continue;
            }
            list.push_back(d);
        }
        return list;
    };

    if (flags & kEnumSessionsAsync) {
        if (!asyncEnum_) {
            asyncEnum_ = true;
            startSearch();
        }
        pump();
        std::vector<SessionDesc> list = selected();
        lock.Unlock();
        for (const SessionDesc &d : list) {
            if (!callback(&d, timeout, 0)) {
                return kDpOk;
            }
        }
        callback(nullptr, timeout, kEscTimedOut);
        return kDpOk;
    }

    for (;;) {
        startSearch();
        std::uint32_t start = now();
        for (;;) {
            pump();
            std::uint32_t elapsed = now() - start;
            if (!haveClock() || elapsed >= timeout) {
                break;
            }
            lock.Wait(std::min(kWaitStepMs, timeout - elapsed));
        }
        enumerating_ = asyncEnum_;
        std::vector<SessionDesc> list = selected();
        lock.Unlock();
        for (const SessionDesc &d : list) {
            if (!callback(&d, timeout, 0)) {
                return kDpOk;
            }
        }
        if (!callback(nullptr, timeout, kEscTimedOut)) {
            return kDpOk;
        }
        if (timeout == 0) {
            timeout = config_.timeoutMs;
        }
        lock.Relock();
    }
}

HResult DirectPlay::Open(const SessionDesc &desc, std::uint32_t flags) {
    Lock lock(*this);
    if (state_ == State::kCreated) {
        return kDpErrUninitialized;
    }
    if (state_ != State::kInitialized) {
        return kDpErrAlreadyInitialized;
    }
    bool create = (flags & kOpenCreate) != 0;
    bool join = (flags & kOpenJoin) != 0;
    if (create == join) {
        return kDpErrInvalidFlags;
    }
    if (create) {
        HResult opened = openSockets(true);
        if (opened != kDpOk) {
            return kDpErrCantCreateSession;
        }
        desc_ = desc;
        desc_.instance = newGuid();
        desc_.flags &= ~kSessionPasswordRequired;
        desc_.currentPlayers = 0;
        key_ = keyOf(desc_.instance);
        self_ = 1;
        host_ = 1;
        nextPeer_ = 2;
        nextLocal_ = 1;
        isHost_ = true;
        state_ = State::kInSession;
        startService();
        return kDpOk;
    }

    auto found = found_.find(desc.instance);
    if (found == found_.end()) {
        return kDpErrNoSessions;
    }
    HResult opened = openSockets(false);
    if (opened != kDpOk) {
        return opened;
    }
    joinInstance_ = desc.instance;
    joinHost_ = found->second.host;
    joinPassword_ = desc.password;
    joinAnswered_ = false;
    joinResult_ = kDpOk;
    state_ = State::kJoining;
    sendTo(joinHost_, wire::encodeJoinRequest(joinInstance_, joinPassword_));
    lastJoinSent_ = now();
    std::uint32_t start = now();
    for (;;) {
        pump();
        std::uint32_t elapsed = now() - start;
        if (joinAnswered_ || !haveClock() || elapsed >= config_.joinTimeoutMs) {
            break;
        }
        lock.Wait(std::min(kWaitStepMs, config_.joinTimeoutMs - elapsed));
    }
    if (!joinAnswered_ || joinResult_ != kDpOk) {
        HResult result = joinAnswered_ ? joinResult_ : kDpErrTimeout;
        closeSession();
        return result;
    }
    startService();
    return kDpOk;
}

HResult DirectPlay::Close() {
    Lock lock(*this);
    if (state_ == State::kInSession || state_ == State::kJoining || state_ == State::kLost) {
        closeSession();
    }
    return kDpOk;
}

HResult DirectPlay::GetSessionDesc(SessionDesc *out) {
    Lock lock(*this);
    if (out == nullptr) {
        return kDpErrInvalidParams;
    }
    if (state_ != State::kInSession) {
        return notInSession();
    }
    pump();
    *out = currentDesc();
    return kDpOk;
}

HResult DirectPlay::SetSessionDesc(const SessionDesc &desc, std::uint32_t) {
    Lock lock(*this);
    if (state_ != State::kInSession) {
        return notInSession();
    }
    if (!isHost_) {
        return kDpErrAccessDenied;
    }
    desc_.flags = desc.flags & ~kSessionPasswordRequired;
    desc_.maxPlayers = desc.maxPlayers;
    desc_.name = desc.name;
    desc_.password = desc.password;
    desc_.user = desc.user;
    broadcastReliable(wire::innerSessionDesc(currentDesc()));
    return kDpOk;
}

HResult DirectPlay::GetCaps(Caps *out, std::uint32_t) {
    Lock lock(*this);
    if (out == nullptr) {
        return kDpErrInvalidParams;
    }
    if (state_ == State::kCreated) {
        return kDpErrUninitialized;
    }
    Caps c;
    c.flags = kCapsKeepAliveOptimized | kCapsGuaranteedOptimized | kCapsGuaranteedSupported |
              kCapsAsyncCancelAllSupported | kCapsAsyncSupported;
    if (state_ == State::kInSession && isHost_) {
        c.flags |= kCapsIsHost;
    }
    c.maxBufferSize = wire::kMaxPayload;
    c.maxQueueSize = 0;
    c.maxPlayers = 0xFFFF;
    c.hundredBaud = 0;
    c.latency = 50;
    c.maxLocalPlayers = 0xFFFF;
    c.headerLength = 28; // the guaranteed data message's header
    c.timeout = config_.timeoutMs;
    *out = c;
    return kDpOk;
}

// ---- players and groups ---------------------------------------------------------------------

HResult DirectPlay::CreatePlayer(Dpid *out, const Name &name, std::uint32_t event,
                                 const std::vector<std::uint8_t> &data, std::uint32_t flags) {
    Lock lock(*this);
    if (out == nullptr) {
        return kDpErrInvalidParams;
    }
    if (state_ != State::kInSession) {
        return notInSession();
    }
    if (flags & ~kPlayerSpectator) {
        return kDpErrInvalidFlags;
    }
    if (desc_.maxPlayers != 0 && playerCount() >= desc_.maxPlayers) {
        return kDpErrCantCreatePlayer;
    }
    wire::Entity e;
    e.id = newId();
    e.flags = flags;
    e.owner = self_;
    e.name = name;
    e.data = data;
    if (event != 0) {
        events_[e.id] = event;
    }
    addEntity(e, true);
    *out = e.id;
    return kDpOk;
}

HResult DirectPlay::DestroyPlayer(Dpid player) {
    Lock lock(*this);
    if (state_ != State::kInSession) {
        return notInSession();
    }
    auto it = entities_.find(player);
    if (it == entities_.end() || it->second.group) {
        return kDpErrInvalidPlayer;
    }
    if (it->second.owner != self_) {
        return kDpErrAccessDenied;
    }
    removeEntity(player, true);
    return kDpOk;
}

HResult DirectPlay::CreateGroup(Dpid *out, const Name &name, const std::vector<std::uint8_t> &data,
                                std::uint32_t flags) {
    Lock lock(*this);
    if (out == nullptr) {
        return kDpErrInvalidParams;
    }
    if (state_ != State::kInSession) {
        return notInSession();
    }
    wire::Entity e;
    e.id = newId();
    e.group = true;
    e.flags = flags;
    e.owner = self_;
    e.name = name;
    e.data = data;
    addEntity(e, true);
    *out = e.id;
    return kDpOk;
}

HResult DirectPlay::DestroyGroup(Dpid group) {
    Lock lock(*this);
    if (state_ != State::kInSession) {
        return notInSession();
    }
    auto it = entities_.find(group);
    if (it == entities_.end() || !it->second.group) {
        return kDpErrInvalidGroup;
    }
    if (it->second.owner != self_) {
        return kDpErrAccessDenied;
    }
    removeEntity(group, true);
    return kDpOk;
}

HResult DirectPlay::AddPlayerToGroup(Dpid group, Dpid player) {
    Lock lock(*this);
    if (state_ != State::kInSession) {
        return notInSession();
    }
    auto g = entities_.find(group);
    if (g == entities_.end() || !g->second.group) {
        return kDpErrInvalidGroup;
    }
    auto p = entities_.find(player);
    if (p == entities_.end() || p->second.group) {
        return kDpErrInvalidPlayer;
    }
    if (g->second.owner != self_ && p->second.owner != self_) {
        return kDpErrAccessDenied; // as on the wire: only the group's or the player's owner
    }
    std::vector<Dpid> &members = g->second.members;
    if (std::find(members.begin(), members.end(), player) != members.end()) {
        return kDpOk;
    }
    members.push_back(player);
    broadcastReliable(wire::innerMember(true, group, player));
    deliverSystem(layoutGroupMembership(kSysAddPlayerToGroup, group, player), 0);
    return kDpOk;
}

HResult DirectPlay::DeletePlayerFromGroup(Dpid group, Dpid player) {
    Lock lock(*this);
    if (state_ != State::kInSession) {
        return notInSession();
    }
    auto g = entities_.find(group);
    if (g == entities_.end() || !g->second.group) {
        return kDpErrInvalidGroup;
    }
    std::vector<Dpid> &members = g->second.members;
    auto m = std::find(members.begin(), members.end(), player);
    if (m == members.end()) {
        return kDpErrInvalidPlayer;
    }
    auto who = entities_.find(player);
    if (g->second.owner != self_ && (who == entities_.end() || who->second.owner != self_)) {
        return kDpErrAccessDenied;
    }
    members.erase(m);
    broadcastReliable(wire::innerMember(false, group, player));
    deliverSystem(layoutGroupMembership(kSysDeletePlayerFromGroup, group, player), 0);
    return kDpOk;
}

HResult DirectPlay::EnumPlayers(const Guid *, const PlayerCallback &callback,
                                std::uint32_t flags) {
    Lock lock(*this);
    if (!callback) {
        return kDpErrInvalidParams;
    }
    if (flags & kEnumPlayersSession) {
        return kDpErrUnsupported; // the players of a session not joined
    }
    if (state_ != State::kInSession) {
        return notInSession();
    }
    pump();
    std::vector<PlayerEntry> list;
    for (const auto &entry : entities_) {
        const wire::Entity &e = entry.second;
        if (e.group && !(flags & kEnumPlayersGroup)) {
            continue;
        }
        bool local = e.owner == self_;
        bool spectator = !e.group && (e.flags & kPlayerSpectator);
        if (((flags & kEnumPlayersLocal) && !local) || ((flags & kEnumPlayersRemote) && local) ||
            ((flags & kEnumPlayersSpectator) && !spectator)) {
            continue;
        }
        PlayerEntry p;
        p.id = e.id;
        p.playerType = e.group ? kPlayerTypeGroup : kPlayerTypePlayer;
        p.name = e.name;
        p.flags = (local ? kEnumPlayersLocal : kEnumPlayersRemote) |
                  (spectator ? kEnumPlayersSpectator : 0);
        list.push_back(p);
    }
    lock.Unlock();
    for (const PlayerEntry &p : list) {
        if (!callback(p)) {
            break;
        }
    }
    return kDpOk;
}

HResult DirectPlay::EnumGroups(const Guid *, const PlayerCallback &callback,
                               std::uint32_t flags) {
    Lock lock(*this);
    if (!callback) {
        return kDpErrInvalidParams;
    }
    if (flags & kEnumPlayersSession) {
        return kDpErrUnsupported;
    }
    if (state_ != State::kInSession) {
        return notInSession();
    }
    pump();
    std::vector<PlayerEntry> list;
    for (const auto &entry : entities_) {
        const wire::Entity &e = entry.second;
        if (!e.group) {
            continue;
        }
        bool local = e.owner == self_;
        if (((flags & kEnumPlayersLocal) && !local) || ((flags & kEnumPlayersRemote) && local)) {
            continue;
        }
        PlayerEntry p;
        p.id = e.id;
        p.playerType = kPlayerTypeGroup;
        p.name = e.name;
        p.flags = local ? kEnumPlayersLocal : kEnumPlayersRemote;
        list.push_back(p);
    }
    lock.Unlock();
    for (const PlayerEntry &p : list) {
        if (!callback(p)) {
            break;
        }
    }
    return kDpOk;
}

HResult DirectPlay::GetPlayerName(Dpid player, Name *out) {
    Lock lock(*this);
    if (out == nullptr) {
        return kDpErrInvalidParams;
    }
    if (state_ != State::kInSession) {
        return notInSession();
    }
    pump();
    auto it = entities_.find(player);
    if (it == entities_.end() || it->second.group) {
        return kDpErrInvalidPlayer;
    }
    *out = it->second.name;
    return kDpOk;
}

HResult DirectPlay::SetPlayerName(Dpid player, const Name &name, std::uint32_t) {
    Lock lock(*this);
    if (state_ != State::kInSession) {
        return notInSession();
    }
    auto it = entities_.find(player);
    if (it == entities_.end() || it->second.group) {
        return kDpErrInvalidPlayer;
    }
    if (it->second.owner != self_) {
        return kDpErrAccessDenied;
    }
    it->second.name = name;
    broadcastReliable(wire::innerSetName(player, name));
    return kDpOk;
}

HResult DirectPlay::GetPlayerData(Dpid player, std::vector<std::uint8_t> *out,
                                  std::uint32_t flags) {
    Lock lock(*this);
    if (out == nullptr) {
        return kDpErrInvalidParams;
    }
    if (state_ != State::kInSession) {
        return notInSession();
    }
    pump();
    auto it = entities_.find(player);
    if (it == entities_.end() || it->second.group) {
        return kDpErrInvalidPlayer;
    }
    if (flags & kGetLocal) {
        auto local = localData_.find(player);
        *out = local == localData_.end() ? std::vector<std::uint8_t>{} : local->second;
    } else {
        *out = it->second.data;
    }
    return kDpOk;
}

HResult DirectPlay::SetPlayerData(Dpid player, const std::vector<std::uint8_t> &data,
                                  std::uint32_t flags) {
    Lock lock(*this);
    if (state_ != State::kInSession) {
        return notInSession();
    }
    auto it = entities_.find(player);
    if (it == entities_.end() || it->second.group) {
        return kDpErrInvalidPlayer;
    }
    if (flags & kSetLocal) {
        localData_[player] = data;
        return kDpOk;
    }
    if (it->second.owner != self_) {
        return kDpErrAccessDenied;
    }
    it->second.data = data;
    // Guaranteed or not, the change travels on the channel, so that every machine keeps the
    // same data.
    broadcastReliable(wire::innerSetData(player, data));
    return kDpOk;
}

HResult DirectPlay::GetPlayerFlags(Dpid player, std::uint32_t *out) {
    Lock lock(*this);
    if (out == nullptr) {
        return kDpErrInvalidParams;
    }
    if (state_ != State::kInSession) {
        return notInSession();
    }
    auto it = entities_.find(player);
    if (it == entities_.end() || it->second.group) {
        return kDpErrInvalidPlayer;
    }
    *out = (it->second.owner == self_ ? kPlayerLocal : 0) | (it->second.flags & kPlayerSpectator);
    return kDpOk;
}

HResult DirectPlay::GetPlayerAddress(Dpid player, std::vector<std::uint8_t> *out) {
    Lock lock(*this);
    if (out == nullptr) {
        return kDpErrInvalidParams;
    }
    if (state_ != State::kInSession) {
        return notInSession();
    }
    auto it = entities_.find(player);
    if (it == entities_.end() || it->second.group) {
        return kDpErrInvalidPlayer;
    }
    platform::Endpoint where;
    if (it->second.owner == self_) {
        Result<platform::Endpoint> local = env_.transport->LocalName(game_);
        if (local.Ok()) {
            where = local.Value();
        }
    } else if (Peer *peer = peerOf(it->second.owner)) {
        where = peer->endpoint;
    }
    *out = tcpIpAddress(where.address, where.port);
    return kDpOk;
}

// ---- messages -------------------------------------------------------------------------------

HResult DirectPlay::Send(Dpid from, Dpid to, std::uint32_t flags, const std::uint8_t *data,
                         std::size_t bytes) {
    return SendEx(from, to, flags & ~kSendAsync, data, bytes, 0, 0, nullptr);
}

HResult DirectPlay::SendEx(Dpid from, Dpid to, std::uint32_t flags, const std::uint8_t *data,
                           std::size_t bytes, std::uint32_t, std::uint32_t,
                           std::uint32_t *messageId) {
    Lock lock(*this);
    if (state_ != State::kInSession) {
        return notInSession();
    }
    if (data == nullptr && bytes != 0) {
        return kDpErrInvalidParams;
    }
    if (bytes > wire::kMaxPayload) {
        return kDpErrSendTooBig;
    }
    pump();
    auto sender = entities_.find(from);
    if (sender == entities_.end() || sender->second.group || sender->second.owner != self_) {
        return kDpErrInvalidPlayer;
    }
    std::set<std::uint32_t> peers;
    if (to == kDpidAllPlayers) {
        for (const auto &entry : peers_) {
            peers.insert(entry.first);
        }
    } else {
        auto target = entities_.find(to);
        if (target == entities_.end()) {
            return kDpErrInvalidPlayer;
        }
        if (!target->second.group) {
            if (target->second.owner != self_) {
                peers.insert(target->second.owner);
            }
        } else {
            for (Dpid member : target->second.members) {
                auto m = entities_.find(member);
                if (m != entities_.end() && m->second.owner != self_) {
                    peers.insert(m->second.owner);
                }
            }
        }
    }
    deliverData(from, to, data, bytes);
    bool guaranteed = (flags & (kSendGuaranteed | kSendSigned | kSendEncrypted)) != 0;
    for (std::uint32_t id : peers) {
        Peer *peer = peerOf(id);
        if (peer == nullptr) {
            continue;
        }
        if (guaranteed) {
            sendReliable(*peer, wire::innerData(from, to, data, bytes),
                         static_cast<std::uint32_t>(bytes));
        } else {
            sendTo(peer->endpoint, wire::encodeUnreliable(key_, self_, from, to, data, bytes));
            peer->lastSent = now();
        }
    }
    if (flags & kSendAsync) {
        std::uint32_t id = nextMessageId_++;
        if (nextMessageId_ == 0) {
            nextMessageId_ = 1;
        }
        if (messageId != nullptr) {
            *messageId = id;
        }
        return kDpErrPending;
    }
    if (messageId != nullptr) {
        *messageId = 0;
    }
    return kDpOk;
}

HResult DirectPlay::Receive(Dpid *from, Dpid *to, std::uint32_t flags, std::uint32_t *size,
                            ReceivedMessage *out) {
    Lock lock(*this);
    if (from == nullptr || to == nullptr || size == nullptr || out == nullptr) {
        return kDpErrInvalidParams;
    }
    if (state_ == State::kCreated) {
        return kDpErrUninitialized;
    }
    pump();
    bool all = (flags & kReceiveAll) || !(flags & (kReceiveToPlayer | kReceiveFromPlayer));
    for (auto it = inbox_.begin(); it != inbox_.end(); ++it) {
        if (!all) {
            if ((flags & kReceiveToPlayer) && it->to != *to) {
                continue;
            }
            if ((flags & kReceiveFromPlayer) && it->from != *from) {
                continue;
            }
        }
        std::uint32_t needed = static_cast<std::uint32_t>(it->blob.bytes.size());
        if (*size < needed) {
            *size = needed;
            return kDpErrBufferTooSmall;
        }
        *from = it->from;
        *to = it->to;
        *size = needed;
        out->from = it->from;
        out->to = it->to;
        out->blob = it->blob;
        if (!(flags & kReceivePeek)) {
            inbox_.erase(it);
        }
        return kDpOk;
    }
    return kDpErrNoMessages;
}

HResult DirectPlay::GetMessageCount(Dpid player, std::uint32_t *count) {
    Lock lock(*this);
    if (count == nullptr) {
        return kDpErrInvalidParams;
    }
    if (state_ != State::kInSession) {
        return notInSession();
    }
    pump();
    if (!isLocal(player)) {
        return kDpErrInvalidPlayer;
    }
    *count = static_cast<std::uint32_t>(
        std::count_if(inbox_.begin(), inbox_.end(), [&](const Queued &q) { return q.to == player; }));
    return kDpOk;
}

HResult DirectPlay::GetMessageQueue(Dpid from, Dpid to, std::uint32_t flags,
                                    std::uint32_t *messages, std::uint32_t *bytes) {
    Lock lock(*this);
    std::uint32_t queue = flags != 0 ? flags : kMessageQueueSend;
    if ((queue & kMessageQueueSend) && (queue & kMessageQueueReceive)) {
        return kDpErrInvalidFlags;
    }
    if (state_ != State::kInSession) {
        return notInSession();
    }
    pump();
    std::uint32_t count = 0;
    std::uint32_t total = 0;
    if (queue & kMessageQueueSend) {
        std::optional<std::uint32_t> only;
        if (to != 0) {
            auto target = entities_.find(to);
            if (target == entities_.end()) {
                return kDpErrInvalidPlayer;
            }
            only = target->second.owner;
        }
        for (const auto &entry : peers_) {
            if (only && entry.first != *only) {
                continue;
            }
            for (const Unacked &u : entry.second.unacked) {
                ++count;
                total += u.payloadBytes;
            }
        }
    } else {
        for (const Queued &q : inbox_) {
            if ((to == 0 || q.to == to) && (from == 0 || q.from == from)) {
                ++count;
                total += static_cast<std::uint32_t>(q.blob.bytes.size());
            }
        }
    }
    if (messages != nullptr) {
        *messages = count;
    }
    if (bytes != nullptr) {
        *bytes = total;
    }
    return kDpOk;
}

HResult DirectPlay::CancelMessage(std::uint32_t, std::uint32_t) { return kDpOk; }

// ---- name table and delivery ----------------------------------------------------------------

void DirectPlay::addEntity(const wire::Entity &entity, bool announce) {
    entities_[entity.id] = entity;
    if (announce) {
        broadcastReliable(wire::innerEntityAdd(entity));
    }
    std::uint32_t type = entity.group ? kPlayerTypeGroup : kPlayerTypePlayer;
    deliverSystem(layoutCreatePlayerOrGroup(type, entity.id, playerCount(), entity.data,
                                            entity.name, entity.flags),
                  entity.id);
}

void DirectPlay::removeEntity(Dpid id, bool announce) {
    auto it = entities_.find(id);
    if (it == entities_.end()) {
        return;
    }
    wire::Entity e = it->second;
    entities_.erase(it);
    for (auto &entry : entities_) {
        std::vector<Dpid> &members = entry.second.members;
        members.erase(std::remove(members.begin(), members.end(), id), members.end());
    }
    std::vector<std::uint8_t> local;
    auto data = localData_.find(id);
    if (data != localData_.end()) {
        local = data->second;
        localData_.erase(data);
    }
    events_.erase(id);
    inbox_.erase(std::remove_if(inbox_.begin(), inbox_.end(),
                                [&](const Queued &q) { return q.to == id; }),
                 inbox_.end());
    if (announce) {
        broadcastReliable(wire::innerEntityRemove(id));
    }
    std::uint32_t type = e.group ? kPlayerTypeGroup : kPlayerTypePlayer;
    deliverSystem(layoutDestroyPlayerOrGroup(type, id, local, e.data, e.name, e.flags), id);
}

void DirectPlay::deliverData(Dpid from, Dpid to, const std::uint8_t *data, std::size_t bytes) {
    std::vector<Dpid> recipients;
    if (to == kDpidAllPlayers) {
        for (Dpid p : localPlayers()) {
            if (p != from) {
                recipients.push_back(p);
            }
        }
    } else {
        auto target = entities_.find(to);
        if (target == entities_.end()) {
            return;
        }
        if (!target->second.group) {
            if (target->second.owner == self_) {
                recipients.push_back(to);
            }
        } else {
            for (Dpid member : target->second.members) {
                if (member != from && isLocal(member)) {
                    recipients.push_back(member);
                }
            }
        }
    }
    for (Dpid r : recipients) {
        Blob blob;
        if (bytes != 0) {
            blob.bytes.assign(data, data + bytes);
        }
        queueFor(r, from, std::move(blob));
    }
}

void DirectPlay::deliverSystem(const Blob &message, Dpid except) {
    for (Dpid p : localPlayers()) {
        if (p != except) {
            queueFor(p, kDpidSysMsg, message);
        }
    }
}

void DirectPlay::queueFor(Dpid player, Dpid from, Blob blob) {
    if (inbox_.size() >= kMaxInbox) {
        inbox_.pop_front();
        ++counters_.droppedMessages;
    }
    inbox_.push_back(Queued{from, player, std::move(blob)});
    auto event = events_.find(player);
    if (event != events_.end()) {
        toSignal_.push_back(event->second);
    }
}

std::uint32_t DirectPlay::playerCount() const {
    return static_cast<std::uint32_t>(
        std::count_if(entities_.begin(), entities_.end(),
                      [](const auto &entry) { return !entry.second.group; }));
}

bool DirectPlay::isLocal(Dpid id) const {
    auto it = entities_.find(id);
    return it != entities_.end() && !it->second.group && it->second.owner == self_;
}

std::vector<Dpid> DirectPlay::localPlayers() const {
    std::vector<Dpid> out;
    for (const auto &entry : entities_) {
        if (!entry.second.group && entry.second.owner == self_) {
            out.push_back(entry.first);
        }
    }
    return out;
}

SessionDesc DirectPlay::currentDesc() const {
    SessionDesc d = desc_;
    d.currentPlayers = playerCount();
    return d;
}

std::vector<wire::Entity> DirectPlay::ownEntities() const {
    std::vector<wire::Entity> out;
    for (const auto &entry : entities_) {
        if (entry.second.owner == self_) {
            out.push_back(entry.second);
        }
    }
    return out;
}

// ---- sockets, session state, service --------------------------------------------------------

HResult DirectPlay::notInSession() const {
    return state_ == State::kLost ? kDpErrSessionLost : kDpErrNoConnection;
}

Counters DirectPlay::GetCounters() {
    Lock lock(*this);
    return counters_;
}

HResult DirectPlay::openSockets(bool host) {
    platform::Transport &t = *env_.transport;
    if (!game_.Valid()) {
        Result<platform::SocketId> s = t.Open(platform::SocketKind::kUdp);
        if (!s.Ok()) {
            return kDpErrNoConnection;
        }
        game_ = s.Value();
        (void)t.SetOption(game_, platform::SocketOption::kNonBlocking, 1);
        (void)t.SetOption(game_, platform::SocketOption::kBroadcast, 1);
        if (!t.Bind(game_, platform::Endpoint{0, 0}).Ok()) {
            (void)t.Close(game_);
            game_ = {};
            return kDpErrNoConnection;
        }
    }
    if (host && !enum_.Valid()) {
        Result<platform::SocketId> s = t.Open(platform::SocketKind::kUdp);
        if (!s.Ok()) {
            return kDpErrCantCreateSession;
        }
        enum_ = s.Value();
        (void)t.SetOption(enum_, platform::SocketOption::kNonBlocking, 1);
        (void)t.SetOption(enum_, platform::SocketOption::kReuseAddress, 1);
        (void)t.SetOption(enum_, platform::SocketOption::kBroadcast, 1);
        if (!t.Bind(enum_, platform::Endpoint{0, config_.enumPort}).Ok()) {
            (void)t.Close(enum_);
            enum_ = {};
            return kDpErrCantCreateSession;
        }
    }
    return kDpOk;
}

void DirectPlay::closeSession() {
    if (key_ != 0) {
        std::vector<std::uint8_t> leave = wire::encodeLeave(key_, self_);
        for (const auto &entry : peers_) {
            sendTo(entry.second.endpoint, leave);
            sendTo(entry.second.endpoint, leave);
        }
    }
    peers_.clear();
    entities_.clear();
    localData_.clear();
    events_.clear();
    inbox_.clear();
    desc_ = SessionDesc{};
    isHost_ = false;
    key_ = 0;
    self_ = 0;
    host_ = 0;
    nextPeer_ = 0;
    joinAnswered_ = false;
    if (enum_.Valid()) {
        (void)env_.transport->Close(enum_);
        enum_ = {};
    }
    if (state_ == State::kInSession || state_ == State::kJoining || state_ == State::kLost) {
        state_ = State::kInitialized;
    }
}

void DirectPlay::startService() {
    if (env_.threads == nullptr || service_) {
        return;
    }
    platform::ThreadOptions options;
    options.name = "dplay";
    Result<platform::Waitable> started = env_.threads->StartThread(
        [this]() -> std::uint32_t {
            serviceStarted_ = true;
            while (!stopService_) {
                Service(50);
            }
            serviceExited_ = true; // the last thing this thread does to the object
            return 0;
        },
        options);
    if (started.Ok()) {
        service_ = started.Value();
    }
}

void DirectPlay::Service(std::uint32_t maxWaitMs) {
    std::vector<platform::SocketId> sockets;
    {
        Lock lock(*this);
        pump();
        sockets = Sockets();
    }
    if (sockets.empty()) {
        (void)env_.clock->Sleep(maxWaitMs);
        return;
    }
    platform::SelectSet set;
    set.readable = sockets;
    std::uint32_t wait = std::min(maxWaitMs, config_.retransmitMs);
    if (!env_.transport->Select(set, wait).Ok()) {
        (void)env_.clock->Sleep(std::min(wait, kWaitStepMs));
    }
    Lock lock(*this);
    pump();
}

std::vector<platform::SocketId> DirectPlay::Sockets() {
    std::vector<platform::SocketId> out;
    if (game_.Valid()) {
        out.push_back(game_);
    }
    if (enum_.Valid()) {
        out.push_back(enum_);
    }
    return out;
}

std::uint32_t DirectPlay::now() { return env_.clock->Ticks().ValueOr(0); }

bool DirectPlay::haveClock() { return env_.clock->Ticks().Ok(); }

void DirectPlay::waitFor(std::uint32_t milliseconds) {
    if (env_.wait) {
        env_.wait(milliseconds);
    } else {
        (void)env_.clock->Sleep(milliseconds);
    }
}

Guid DirectPlay::newGuid() {
    Guid g;
    std::uint64_t a = splitmix(random_);
    std::uint64_t b = splitmix(random_);
    for (int i = 0; i < 8; ++i) {
        g.bytes[i] = static_cast<std::uint8_t>(a >> (8 * i));
        g.bytes[8 + i] = static_cast<std::uint8_t>(b >> (8 * i));
    }
    g.bytes[7] = static_cast<std::uint8_t>((g.bytes[7] & 0x0F) | 0x40); // version 4
    g.bytes[8] = static_cast<std::uint8_t>((g.bytes[8] & 0x3F) | 0x80); // variant
    return g;
}

Dpid DirectPlay::newId() {
    for (;;) {
        Dpid id = self_ << 16 | nextLocal_;
        if (++nextLocal_ == 0) {
            nextLocal_ = 1;
        }
        if (entities_.count(id) == 0) {
            return id;
        }
    }
}

} // namespace sfcop::hle::dplay
