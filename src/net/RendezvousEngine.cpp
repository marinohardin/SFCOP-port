#include "net/RendezvousEngine.h"

#include "net/Bytes.h"

#include <algorithm>

namespace sfcop::net {
namespace {

using platform::Endpoint;
using platform::Error;

// RENDEZVOUS.md, "The punch sequence".
constexpr std::uint32_t kAskRepeatMs = 250;
constexpr std::uint32_t kServiceAnswerMs = 500;
constexpr std::uint32_t kProbeSpacingMs = 20;
constexpr std::uint32_t kProbeRepeatMs = 100;
constexpr std::uint32_t kAnswerAfterChoiceMs = 500;
constexpr std::uint32_t kRelayAfterMs = 3000;
constexpr std::uint32_t kGiveUpAfterMs = 4500;
constexpr std::uint32_t kRelayPingRepeatMs = 250;
// A second introduction to a peer already open answers its probes for as long as its punch can
// run (its relay timer starts when its own introduction arrived).
constexpr std::uint32_t kAnswerMergedMs = kRelayAfterMs + kAnswerAfterChoiceMs;
constexpr std::size_t kReceiveBuffer = 2048;
// kConnectionReset answers (ICMP for an earlier datagram) skipped in one Pump before it stops.
constexpr std::size_t kMaxResetsPerPump = 8;

bool reached(std::uint32_t now, std::uint32_t due) {
    return static_cast<std::int32_t>(now - due) >= 0;
}

bool contains(const std::vector<Endpoint> &list, Endpoint endpoint) {
    return std::find(list.begin(), list.end(), endpoint) != list.end();
}

} // namespace

Error errorForResult(RvResult result) {
    switch (result) {
    case RvResult::kOk:
        return Error::kNone;
    case RvResult::kUnknownSession:
        return Error::kNotFound;
    case RvResult::kUnknownPeer:
        return Error::kHostUnreachable;
    case RvResult::kBadTicket:
    case RvResult::kTicketExpired:
        return Error::kAccessDenied;
    case RvResult::kFull:
    case RvResult::kRateLimited:
        return Error::kOutOfResources;
    case RvResult::kUnsupportedVersion:
        return Error::kUnsupported;
    case RvResult::kUnknownBinding:
        // Answered by binding again; a call waiting on that Bind reports the Bind's own error.
        return Error::kWrongState;
    }
    return Error::kIoError;
}

std::vector<std::uint32_t> lookUpLocalAddresses(platform::Transport &transport) {
    std::vector<std::uint32_t> found;
    platform::Result<std::string> host = transport.HostName();
    if (!host.Ok()) {
        return found;
    }
    platform::Result<std::vector<std::uint32_t>> addresses = transport.Resolve(host.Value());
    if (!addresses.Ok()) {
        return found;
    }
    for (std::uint32_t address : addresses.Value()) {
        if (address != 0 && (address >> 24) != 127 &&
            std::find(found.begin(), found.end(), address) == found.end()) {
            found.push_back(address);
        }
    }
    return found;
}

RendezvousEngine::RendezvousEngine(platform::Transport &transport, RendezvousSettings settings)
    : transport_(transport), settings_(settings), nextTxn_(settings.seed == 0 ? 1 : settings.seed) {
}

RendezvousEngine::~RendezvousEngine() {
    if (socket_.Valid()) {
        (void)transport_.Close(socket_);
    }
}

platform::Status RendezvousEngine::openSocket() {
    if (socket_.Valid()) {
        return {};
    }
    platform::Result<platform::SocketId> opened = transport_.Open(platform::SocketKind::kUdp);
    if (!opened.Ok()) {
        return opened.Code();
    }
    platform::SocketId socket = opened.Value();
    platform::Status ready = transport_.SetOption(socket, platform::SocketOption::kNonBlocking, 1);
    if (ready.Ok()) {
        ready = transport_.Bind(socket, Endpoint{0, 0});
    }
    if (!ready.Ok()) {
        (void)transport_.Close(socket);
        return ready.Code();
    }
    socket_ = socket;
    return {};
}

void RendezvousEngine::sendTo(Endpoint to, const std::vector<std::uint8_t> &bytes) {
    // A datagram that cannot leave now is lost, as UDP's would be; the timers send again.
    (void)transport_.SendTo(socket_, bytes.data(), bytes.size(), to);
}

void RendezvousEngine::sendRv(Endpoint to, const RvMessage &message) {
    sendTo(to, encodeRendezvous(message));
}

void RendezvousEngine::sendBind(std::uint32_t nowMs) {
    RvMessage bind;
    bind.type = RvType::kBind;
    bind.transaction = bindTxn_ = nextTxn_++;
    bind.ticket = ticket_;
    if (!settings_.localAddresses.empty()) {
        platform::Result<Endpoint> local = transport_.LocalName(socket_);
        if (local.Ok()) {
            for (std::uint32_t address : settings_.localAddresses) {
                if (bind.candidates.size() == kMaxBindCandidates) {
                    break;
                }
                bind.candidates.push_back(
                    Candidate{address, local.Value().port, CandidateKind::kLocal});
            }
        }
    }
    sendRv(settings_.service, bind);
    lastBindMs_ = nowMs;
    bindInFlight_ = true;
}

platform::Status RendezvousEngine::StartBind(std::uint64_t sessionId, std::uint32_t peerId,
                                             const Ticket &ticket, std::uint32_t nowMs) {
    platform::Status opened = openSocket();
    if (!opened.Ok()) {
        return opened;
    }
    if (bindState_ == BindState::kBound && (sessionId != session_ || peerId != self_)) {
        (void)Unbind(nowMs);
    }
    session_ = sessionId;
    self_ = peerId;
    ticket_ = ticket;
    bindState_ = BindState::kBinding;
    bindError_ = Error::kNone;
    bindStartMs_ = nowMs;
    sendBind(nowMs);
    return {};
}

void RendezvousEngine::AbandonBind(std::uint32_t nowMs) {
    (void)nowMs;
    if (bindState_ == BindState::kBinding) {
        bindState_ = BindState::kFailed;
        bindError_ = Error::kTimedOut;
        bindInFlight_ = false;
    }
}

platform::Status RendezvousEngine::Unbind(std::uint32_t nowMs) {
    (void)nowMs;
    if (bindState_ != BindState::kBound) {
        return Error::kWrongState;
    }
    RvMessage unbind;
    unbind.type = RvType::kUnbind;
    unbind.transaction = nextTxn_++;
    unbind.sessionId = session_;
    unbind.peer = self_;
    sendRv(settings_.service, unbind);
    bindState_ = BindState::kUnbound;
    bindInFlight_ = false;
    return {};
}

RendezvousEngine::Peer &RendezvousEngine::newPeer(std::uint32_t id, std::uint32_t nowMs) {
    peers_.erase(id);
    Peer &peer = peers_[id];
    peer.id = id;
    peer.clockStartMs = nowMs;
    peer.answerUntilMs = nowMs; // closed until a direct path is chosen
    peer.link = std::make_unique<PeerLink>(session_, self_, id, nowMs);
    return peer;
}

void RendezvousEngine::sendConnect(Peer &peer, std::uint32_t nowMs) {
    RvMessage connect;
    connect.type = RvType::kConnect;
    connect.transaction = peer.connectTxn = nextTxn_++;
    connect.sessionId = session_;
    connect.source = self_;
    connect.target = peer.id;
    sendRv(settings_.service, connect);
    peer.lastAskMs = nowMs;
}

platform::Result<RendezvousEngine::OpId> RendezvousEngine::StartConnect(std::uint32_t id,
                                                                        std::uint32_t nowMs) {
    if (bindState_ != BindState::kBound) {
        return Error::kWrongState;
    }
    if (id == 0 || id == self_) {
        return Error::kInvalidArgument;
    }
    const OpId op = nextOp_++;
    auto found = peers_.find(id);
    if (found != peers_.end()) {
        Peer &peer = found->second;
        const bool gone = peer.phase == Phase::kOpen && Gone(id, nowMs);
        if (peer.op != 0 || (peer.owned && !gone)) {
            return Error::kWrongState;
        }
        if (peer.queued && !gone) {
            // The peer reached this machine first; this call takes its socket.
            accepted_.erase(std::remove(accepted_.begin(), accepted_.end(), id), accepted_.end());
            peer.queued = false;
            ops_[op] = OpStatus{true, Error::kNone, id};
            peer.op = op;
            return op;
        }
        if (!gone && !peer.releasing && peer.phase != Phase::kFailed) {
            // Its introduction is under way: the two calls met; this one waits for that punch.
            peer.op = op;
            ops_[op] = OpStatus{false, Error::kNone, id};
            return op;
        }
    }
    Peer &peer = newPeer(id, nowMs);
    peer.op = op;
    peer.askStartMs = nowMs;
    ops_[op] = OpStatus{false, Error::kNone, id};
    sendConnect(peer, nowMs);
    return op;
}

RendezvousEngine::OpStatus RendezvousEngine::Op(OpId op) const {
    auto found = ops_.find(op);
    return found == ops_.end() ? OpStatus{true, Error::kWrongState, 0} : found->second;
}

void RendezvousEngine::FinishOp(OpId op) {
    auto found = ops_.find(op);
    if (found == ops_.end()) {
        return;
    }
    auto peer = peers_.find(found->second.peer);
    if (peer != peers_.end() && peer->second.op == op) {
        peer->second.op = 0;
        if (found->second.error == Error::kNone) {
            peer->second.owned = true;
        }
    }
    ops_.erase(found);
}

void RendezvousEngine::CancelOp(OpId op, std::uint32_t nowMs) {
    auto found = ops_.find(op);
    if (found == ops_.end()) {
        return;
    }
    auto peer = peers_.find(found->second.peer);
    ops_.erase(found);
    if (peer == peers_.end() || peer->second.op != op) {
        return;
    }
    peer->second.op = 0;
    if (peer->second.phase == Phase::kOpen) {
        Release(peer->first, nowMs);
    } else if (!peer->second.incoming) {
        peers_.erase(peer);
    }
}

std::optional<std::uint32_t> RendezvousEngine::TakeAccepted() {
    while (!accepted_.empty()) {
        std::uint32_t id = accepted_.front();
        accepted_.pop_front();
        auto found = peers_.find(id);
        if (found != peers_.end() && found->second.queued) {
            found->second.queued = false;
            found->second.owned = true;
            return id;
        }
    }
    return std::nullopt;
}

const RendezvousEngine::Peer *RendezvousEngine::ownedPeer(std::uint32_t id) const {
    auto found = peers_.find(id);
    if (found == peers_.end() || !found->second.owned || !found->second.link) {
        return nullptr;
    }
    return &found->second;
}

platform::Status RendezvousEngine::Send(std::uint32_t id, const std::uint8_t *data,
                                        std::size_t bytes, std::uint32_t nowMs) {
    auto found = peers_.find(id);
    if (found == peers_.end() || !found->second.owned) {
        return Error::kNotConnected;
    }
    std::vector<OutgoingEnvelope> out;
    platform::Status sent = found->second.link->Send(data, bytes, false, nowMs, out);
    flush(found->second, out);
    return sent;
}

bool RendezvousEngine::Readable(std::uint32_t id) const {
    const Peer *peer = ownedPeer(id);
    return peer && peer->link->HasMessage();
}

std::size_t RendezvousEngine::NextMessageBytes(std::uint32_t id) const {
    const Peer *peer = ownedPeer(id);
    return peer ? peer->link->NextMessageBytes() : 0;
}

std::vector<std::uint8_t> RendezvousEngine::TakeMessage(std::uint32_t id) {
    auto found = peers_.find(id);
    if (found == peers_.end() || !found->second.link || !found->second.link->HasMessage()) {
        return {};
    }
    return found->second.link->TakeMessage();
}

bool RendezvousEngine::Gone(std::uint32_t id, std::uint32_t nowMs) const {
    auto found = peers_.find(id);
    if (found == peers_.end() || !found->second.link) {
        return true;
    }
    const PeerLink &link = *found->second.link;
    return link.Dead(nowMs) || link.PeerClosed();
}

void RendezvousEngine::Release(std::uint32_t id, std::uint32_t nowMs) {
    auto found = peers_.find(id);
    if (found == peers_.end()) {
        return;
    }
    Peer &peer = found->second;
    peer.owned = false;
    if (peer.phase != Phase::kOpen || Gone(id, nowMs)) {
        peers_.erase(found);
        return;
    }
    std::vector<OutgoingEnvelope> out;
    peer.link->Close(0, nowMs, out);
    flush(peer, out);
    peer.releasing = true;
}

PathKind RendezvousEngine::Path(std::uint32_t id) const {
    auto found = peers_.find(id);
    return found == peers_.end() ? PathKind::kNone : found->second.path;
}

Endpoint RendezvousEngine::PathEndpoint(std::uint32_t id) const {
    auto found = peers_.find(id);
    if (found == peers_.end()) {
        return Endpoint{};
    }
    return found->second.path == PathKind::kRelay ? settings_.service : found->second.direct;
}

void RendezvousEngine::flush(Peer &peer, std::vector<OutgoingEnvelope> &out) {
    for (const OutgoingEnvelope &envelope : out) {
        Route route = envelope.route;
        if (route == Route::kChosen) {
            if (peer.path == PathKind::kNone) {
                continue; // nothing chosen yet; the link's timers send again
            }
            route = peer.path == PathKind::kRelay ? Route::kRelay : Route::kDirect;
        }
        if (route == Route::kRelay) {
            sendTo(settings_.service, envelope.bytes);
        } else if (peer.direct.port != 0) {
            sendTo(peer.direct, envelope.bytes);
        }
    }
    out.clear();
}

void RendezvousEngine::addTarget(Peer &peer, Endpoint endpoint, std::uint32_t firstMs) {
    for (const ProbeTarget &target : peer.targets) {
        if (target.endpoint == endpoint) {
            return;
        }
    }
    peer.targets.push_back(ProbeTarget{endpoint, firstMs});
}

void RendezvousEngine::startPunch(Peer &peer, const RvMessage &message, std::uint32_t nowMs) {
    peer.punchIds.push_back(message.punchId);
    for (std::size_t i = 0; i < message.candidates.size(); ++i) {
        const Candidate &candidate = message.candidates[i];
        addTarget(peer, Endpoint{candidate.address, candidate.port},
                  nowMs + static_cast<std::uint32_t>(i) * kProbeSpacingMs);
    }
    if (settings_.forceRelay) {
        peer.phase = Phase::kRelayOpening;
        peer.relayPinged = false;
    } else {
        peer.phase = Phase::kPunching;
    }
}

void RendezvousEngine::choose(Peer &peer, PathKind path, Endpoint direct, std::uint32_t nowMs) {
    peer.phase = Phase::kOpen;
    peer.path = path;
    if (path == PathKind::kDirect) {
        peer.direct = direct;
        peer.answerUntilMs = nowMs + kAnswerAfterChoiceMs;
    }
    if (peer.op != 0) {
        ops_[peer.op] = OpStatus{true, Error::kNone, peer.id};
    } else if (!peer.owned && !peer.queued) {
        peer.queued = true;
        accepted_.push_back(peer.id);
    }
}

void RendezvousEngine::fail(Peer &peer, Error error) {
    if (peer.phase == Phase::kFailed) {
        return;
    }
    peer.phase = Phase::kFailed;
    peer.failure = error;
    if (peer.op != 0) {
        ops_[peer.op] = OpStatus{true, error, peer.id};
        peer.op = 0;
    } else if (peer.incoming) {
        ++acceptFailures_;
    }
}

void RendezvousEngine::Pump(std::uint32_t nowMs) {
    if (socket_.Valid()) {
        std::uint8_t buffer[kReceiveBuffer];
        std::size_t taken = 0;
        std::size_t resets = 0;
        while (taken < kMaxDatagramsPerPump && resets < kMaxResetsPerPump) {
            platform::Result<platform::Received> got =
                transport_.ReceiveFrom(socket_, buffer, sizeof buffer);
            if (!got.Ok()) {
                if (got.Code() == Error::kConnectionReset) {
                    ++resets; // an ICMP answer to an earlier datagram; the next one may be data
                    continue;
                }
                break;
            }
            ++taken;
            onDatagram(got.Value().from, buffer, got.Value().bytes, nowMs);
        }
    }

    if (bindState_ == BindState::kBinding) {
        if (nowMs - bindStartMs_ >= kServiceAnswerMs) {
            bindState_ = BindState::kFailed;
            bindError_ = Error::kTimedOut;
            bindInFlight_ = false;
            bindFinished(nowMs);
        } else if (nowMs - lastBindMs_ >= kAskRepeatMs) {
            sendBind(nowMs);
        }
    } else if (bindState_ == BindState::kBound && nowMs - lastRefreshMs_ >= refreshMs_) {
        sendBind(nowMs);
        lastRefreshMs_ = nowMs;
    }

    for (auto &[id, peer] : peers_) {
        (void)id;
        runPeer(peer, nowMs);
    }
    for (auto it = peers_.begin(); it != peers_.end();) {
        const Peer &peer = it->second;
        bool forget = (peer.phase == Phase::kFailed && peer.op == 0) ||
                      (peer.releasing && peer.link->CloseFinished(nowMs)) ||
                      (!peer.owned && !peer.queued && peer.op == 0 && peer.phase == Phase::kOpen &&
                       Gone(it->first, nowMs));
        it = forget ? peers_.erase(it) : std::next(it);
    }
}

void RendezvousEngine::runPeer(Peer &peer, std::uint32_t nowMs) {
    std::vector<OutgoingEnvelope> out;
    switch (peer.phase) {
    case Phase::kAskingService:
        if (peer.waitingForRebind) {
            break;
        }
        if (nowMs - peer.askStartMs >= kServiceAnswerMs) {
            fail(peer, Error::kTimedOut);
        } else if (nowMs - peer.lastAskMs >= kAskRepeatMs) {
            sendConnect(peer, nowMs);
        }
        break;
    case Phase::kPunching:
        if (nowMs - peer.clockStartMs >= kRelayAfterMs) {
            peer.phase = Phase::kRelayOpening;
            peer.relayPinged = false;
            runPeer(peer, nowMs);
            return;
        }
        for (ProbeTarget &target : peer.targets) {
            if (!reached(nowMs, target.nextMs)) {
                continue;
            }
            RvMessage probe;
            probe.type = RvType::kProbe;
            probe.transaction = nextTxn_++;
            probe.sessionId = session_;
            probe.source = self_;
            probe.target = peer.id;
            probe.punchId = peer.punchIds.front();
            sendRv(target.endpoint, probe);
            target.nextMs = nowMs + kProbeRepeatMs;
        }
        break;
    case Phase::kRelayOpening:
        if (peer.link->PongSeen(Route::kRelay)) {
            choose(peer, PathKind::kRelay, Endpoint{}, nowMs);
        } else if (nowMs - peer.clockStartMs >= kGiveUpAfterMs) {
            fail(peer, Error::kHostUnreachable);
        } else if (!peer.relayPinged || nowMs - peer.lastRelayPingMs >= kRelayPingRepeatMs) {
            peer.link->Ping(Route::kRelay, nowMs, out);
            peer.relayPinged = true;
            peer.lastRelayPingMs = nowMs;
        }
        break;
    case Phase::kOpen:
        peer.link->Tick(nowMs, out);
        break;
    case Phase::kFailed:
        break;
    }
    flush(peer, out);
}

bool RendezvousEngine::Idle() const {
    return bindState_ != BindState::kBinding && bindState_ != BindState::kBound && peers_.empty() &&
           ops_.empty();
}

void RendezvousEngine::onDatagram(Endpoint from, const std::uint8_t *data, std::size_t bytes,
                                  std::uint32_t nowMs) {
    const std::uint32_t magic = datagramMagic(data, bytes);
    if (magic == kEnvelopeMagic) {
        onEnvelope(from, data, bytes, nowMs);
        return;
    }
    std::optional<RvMessage> message;
    if (magic == kRendezvousMagic) {
        message = decodeRendezvous(data, bytes);
    }
    if (!message) {
        ++refused_;
        return;
    }
    if (from == settings_.service) {
        onService(*message, nowMs);
    } else if (message->type == RvType::kProbe) {
        onProbe(from, *message, nowMs);
    } else if (message->type == RvType::kProbeReply) {
        onProbeReply(from, *message, nowMs);
    } else {
        ++refused_;
    }
}

void RendezvousEngine::onService(const RvMessage &message, std::uint32_t nowMs) {
    switch (message.type) {
    case RvType::kBindReply:
        onBindAnswer(message, nowMs);
        return;
    case RvType::kPeerCandidates:
        onCandidates(message, nowMs);
        return;
    case RvType::kError:
        break;
    default:
        ++refused_;
        return;
    }
    if (bindInFlight_ && message.transaction == bindTxn_) {
        onBindAnswer(message, nowMs);
        return;
    }
    for (auto &[id, peer] : peers_) {
        (void)id;
        if (peer.phase != Phase::kAskingService || peer.connectTxn != message.transaction) {
            continue;
        }
        if (message.result == RvResult::kUnknownBinding) {
            // The service forgot this machine (a restart): bind again, then ask again.
            peer.waitingForRebind = true;
            if (!bindInFlight_) {
                bindState_ = BindState::kBinding;
                bindStartMs_ = nowMs;
                sendBind(nowMs);
            }
        } else {
            fail(peer, errorForResult(message.result));
        }
        return;
    }
}

void RendezvousEngine::onBindAnswer(const RvMessage &message, std::uint32_t nowMs) {
    if (!bindInFlight_ || message.transaction != bindTxn_) {
        return; // an answer to a Bind already answered, or superseded
    }
    bindInFlight_ = false;
    if (message.type == RvType::kBindReply && message.result == RvResult::kOk) {
        bindState_ = BindState::kBound;
        bindError_ = Error::kNone;
        refreshMs_ = (message.refreshSeconds == 0 ? 15u : message.refreshSeconds) * 1000u;
        lastRefreshMs_ = nowMs;
    } else if (bindState_ != BindState::kUnbound) {
        bindState_ = BindState::kFailed;
        bindError_ = errorForResult(message.result);
    }
    bindFinished(nowMs);
}

void RendezvousEngine::bindFinished(std::uint32_t nowMs) {
    for (auto &[id, peer] : peers_) {
        (void)id;
        if (!peer.waitingForRebind) {
            continue;
        }
        peer.waitingForRebind = false;
        if (bindState_ == BindState::kBound) {
            peer.askStartMs = nowMs;
            sendConnect(peer, nowMs);
        } else {
            // The rebind failed: the call reports why (kAccessDenied for an expired ticket,
            // kTimedOut for no answer), as Bind() would.
            fail(peer, bindError_ == Error::kNone ? Error::kWrongState : bindError_);
        }
    }
}

void RendezvousEngine::onCandidates(const RvMessage &message, std::uint32_t nowMs) {
    if (bindState_ != BindState::kBound || message.sessionId != session_ || message.peer == 0 ||
        message.peer == self_ || message.punchId == 0) {
        ++refused_;
        return;
    }
    auto found = peers_.find(message.peer);
    if (found != peers_.end()) {
        Peer &peer = found->second;
        if (std::find(peer.punchIds.begin(), peer.punchIds.end(), message.punchId) !=
            peer.punchIds.end()) {
            return; // the second copy
        }
        switch (peer.phase) {
        case Phase::kAskingService:
            startPunch(peer, message, nowMs);
            return;
        case Phase::kPunching:
        case Phase::kRelayOpening:
            peer.punchIds.push_back(message.punchId);
            for (const Candidate &candidate : message.candidates) {
                addTarget(peer, Endpoint{candidate.address, candidate.port}, nowMs);
            }
            return;
        case Phase::kOpen:
            if (!Gone(peer.id, nowMs) && !peer.releasing) {
                // The other side is reaching this machine again while the path stands: answer
                // its probes, keep the path.
                peer.punchIds.push_back(message.punchId);
                peer.answerUntilMs = nowMs + kAnswerMergedMs;
                return;
            }
            break;
        case Phase::kFailed:
            break;
        }
        if (peer.op != 0 || peer.owned) {
            // A socket still holds the dead link; the new introduction starts a fresh one once
            // that socket is closed.
            return;
        }
    }
    Peer &peer = newPeer(message.peer, nowMs);
    peer.incoming = true;
    startPunch(peer, message, nowMs);
}

void RendezvousEngine::onProbe(Endpoint from, const RvMessage &message, std::uint32_t nowMs) {
    auto found = peers_.find(message.source);
    if (settings_.forceRelay || message.sessionId != session_ || message.target != self_ ||
        found == peers_.end()) {
        ++refused_;
        return;
    }
    Peer &peer = found->second;
    if (std::find(peer.punchIds.begin(), peer.punchIds.end(), message.punchId) ==
        peer.punchIds.end()) {
        return; // a wrong punch id: dropped silently
    }
    bool answering = peer.phase == Phase::kPunching || peer.phase == Phase::kRelayOpening ||
                     (peer.phase == Phase::kOpen && !reached(nowMs, peer.answerUntilMs));
    if (!answering) {
        return;
    }
    RvMessage reply = message;
    reply.type = RvType::kProbeReply;
    reply.source = self_;
    reply.target = message.source;
    sendRv(from, reply);
    if (!contains(peer.learned, from)) {
        peer.learned.push_back(from);
    }
    if (peer.phase == Phase::kPunching) {
        addTarget(peer, from, nowMs);
    }
}

void RendezvousEngine::onProbeReply(Endpoint from, const RvMessage &message, std::uint32_t nowMs) {
    auto found = peers_.find(message.source);
    if (message.sessionId != session_ || message.target != self_ || found == peers_.end()) {
        ++refused_;
        return;
    }
    Peer &peer = found->second;
    if (peer.phase != Phase::kPunching || std::find(peer.punchIds.begin(), peer.punchIds.end(),
                                                    message.punchId) == peer.punchIds.end()) {
        return;
    }
    if (!contains(peer.learned, from)) {
        peer.learned.push_back(from);
    }
    choose(peer, PathKind::kDirect, from, nowMs);
}

bool RendezvousEngine::knowsAddress(const Peer &peer, Endpoint from) const {
    return (peer.path == PathKind::kDirect && peer.direct == from) || contains(peer.learned, from);
}

void RendezvousEngine::onEnvelope(Endpoint from, const std::uint8_t *data, std::size_t bytes,
                                  std::uint32_t nowMs) {
    std::optional<Envelope> envelope = decodeEnvelope(data, bytes);
    // Gated on the session and the peer, not on the binding: sockets handed out stay open after
    // Unbind() (Transport.h), so an existing peer's envelopes still arrive. An envelope that would
    // make a new peer (relayed, with no introduction) needs the binding: it is refused unless
    // kBound.
    if (!envelope || self_ == 0 || envelope->header.sessionId != session_ ||
        envelope->header.destination != self_) {
        ++refused_;
        return;
    }
    const std::uint32_t source = envelope->header.source;
    const bool relayed = from == settings_.service;
    auto found = peers_.find(source);
    if (found == peers_.end()) {
        if (!relayed || source == 0 || source == self_ || bindState_ != BindState::kBound) {
            ++refused_;
            return;
        }
        // Through the relay from a peer of this session that this machine has no introduction
        // for (both copies of its PeerCandidates were lost): the relay vouches for it.
        Peer &peer = newPeer(source, nowMs);
        peer.incoming = true;
        choose(peer, PathKind::kRelay, Endpoint{}, nowMs);
        found = peers_.find(source);
    }
    Peer &peer = found->second;
    if (!relayed && !knowsAddress(peer, from)) {
        ++refused_;
        return;
    }
    if (peer.phase == Phase::kFailed) {
        return;
    }
    if (peer.phase != Phase::kOpen) {
        // RENDEZVOUS.md step 6: an envelope from the other peer before this side chose a path
        // chooses the path it came on.
        if (relayed) {
            choose(peer, PathKind::kRelay, Endpoint{}, nowMs);
        } else {
            choose(peer, PathKind::kDirect, from, nowMs);
        }
    }
    std::vector<OutgoingEnvelope> out;
    peer.link->Receive(*envelope, relayed ? Route::kRelay : Route::kDirect, nowMs, out);
    flush(peer, out);
}

} // namespace sfcop::net
