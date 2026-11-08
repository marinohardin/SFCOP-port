// The rendezvous client of RENDEZVOUS.md as a state machine: binding and its refresh, the punch
// sequence (connect, candidates, probes, the choice of a path), the relay fallback, and one
// PeerLink per peer for the envelopes that follow. It runs on one UDP socket of the Transport it
// is given, the socket its binding names, so that every envelope leaves through the NAT mapping
// the service saw.
//
// It never blocks and never reads a clock: Pump(now) reads what has arrived and runs the timers,
// and every other call takes the time too. RendezvousTransport drives it from a thread with the
// steady clock and turns its operations into the blocking calls of platform::Rendezvous; the tests
// drive it with simulated time. Not thread-safe; its owner serialises calls.
#pragma once

#include "net/PeerLink.h"
#include "net/RendezvousMessages.h"
#include "platform/Transport/Transport.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace sfcop::net {

struct RendezvousSettings {
    platform::Endpoint service; // the rendezvous and relay port
    bool forceRelay = false;    // RENDEZVOUS.md step 7: send no probes, take the relay at once
    // This machine's interface addresses, offered in Bind as local candidates; empty offers none.
    // The engine never looks them up itself: lookUpLocalAddresses() may block.
    std::vector<std::uint32_t> localAddresses;
    std::uint32_t seed = 1; // the first transaction number
};

enum class PathKind : std::uint8_t { kNone, kDirect, kRelay };

// The service's answer as the Transport contract reports it (Transport.h, "Service answers").
platform::Error errorForResult(RvResult result);

// This machine's IPv4 interface addresses worth offering as local candidates (not 0, not
// loopback), through the Transport's HostName() and Resolve(). May block for seconds, as Resolve()
// may: call it where nothing waits on the engine.
std::vector<std::uint32_t> lookUpLocalAddresses(platform::Transport &transport);

class RendezvousEngine {
public:
    enum class BindState : std::uint8_t { kUnbound, kBinding, kBound, kFailed };
    using OpId = std::uint32_t;

    struct OpStatus {
        bool done = false;
        platform::Error error = platform::Error::kNone; // kNone: the peer is reachable
        std::uint32_t peer = 0;
    };

    // Datagrams Pump() reads at most, so that a flood cannot hold its caller.
    static constexpr std::size_t kMaxDatagramsPerPump = 64;

    RendezvousEngine(platform::Transport &transport, RendezvousSettings settings);
    ~RendezvousEngine();
    RendezvousEngine(const RendezvousEngine &) = delete;
    RendezvousEngine &operator=(const RendezvousEngine &) = delete;

    // Opens the socket if needed and sends Bind. A binding for another session or peer is
    // unbound first. Errors: the Transport's, from opening the socket.
    platform::Status StartBind(std::uint64_t sessionId, std::uint32_t peerId, const Ticket &ticket,
                               std::uint32_t nowMs);
    BindState Binding() const { return bindState_; }
    platform::Error BindError() const { return bindError_; }
    // A Bind call that gave up before the answer: the binding is dropped.
    void AbandonBind(std::uint32_t nowMs);
    // Sends Unbind and stops the refresh. Errors: kWrongState if not bound.
    platform::Status Unbind(std::uint32_t nowMs);
    // Replaces the local candidates the next Bind (first, refresh or rebind) offers.
    void SetLocalAddresses(std::vector<std::uint32_t> addresses) {
        settings_.localAddresses = std::move(addresses);
    }
    std::uint64_t Session() const { return session_; }
    std::uint32_t Self() const { return self_; }

    // Starts reaching `peer` of the bound session. Errors: kWrongState if not bound, or a socket
    // to that peer is open or a call is already reaching it; kInvalidArgument for this machine's
    // own peer id or 0.
    platform::Result<OpId> StartConnect(std::uint32_t peer, std::uint32_t nowMs);
    OpStatus Op(OpId op) const;
    // Takes a finished operation's result; on success the peer's link belongs to the caller.
    void FinishOp(OpId op);
    // Drops an operation whose caller gave up.
    void CancelOp(OpId op, std::uint32_t nowMs);

    // A peer that reached this machine, now the caller's; or nothing.
    std::optional<std::uint32_t> TakeAccepted();
    // Counts the peers that reached this machine and for which no path was found.
    std::uint32_t AcceptFailures() const { return acceptFailures_; }

    // The peer's link, once the caller owns it.
    platform::Status Send(std::uint32_t peer, const std::uint8_t *data, std::size_t bytes,
                          std::uint32_t nowMs);
    bool Readable(std::uint32_t peer) const;
    std::size_t NextMessageBytes(std::uint32_t peer) const;
    std::vector<std::uint8_t> TakeMessage(std::uint32_t peer);
    // The path died or the peer closed it.
    bool Gone(std::uint32_t peer, std::uint32_t nowMs) const;
    // The caller closed its socket: the link sends EnvelopeClose and is forgotten afterwards.
    void Release(std::uint32_t peer, std::uint32_t nowMs);
    PathKind Path(std::uint32_t peer) const;
    // Where the chosen path sends: the peer's direct address, or the service for the relay.
    platform::Endpoint PathEndpoint(std::uint32_t peer) const;

    // Reads the datagrams waiting on the socket, at most kMaxDatagramsPerPump, and runs the
    // timers.
    void Pump(std::uint32_t nowMs);
    // Nothing to pump for: no binding held or being made, and no peer (open, closing or being
    // reached). Pump() would send nothing and change nothing until StartBind().
    bool Idle() const;

    // The UDP socket of the binding (invalid before the first StartBind()).
    platform::SocketId Socket() const { return socket_; }
    // Datagrams refused: malformed, for another session, or from an address not on a path.
    std::uint32_t Refused() const { return refused_; }

private:
    enum class Phase : std::uint8_t { kAskingService, kPunching, kRelayOpening, kOpen, kFailed };

    struct ProbeTarget {
        platform::Endpoint endpoint;
        std::uint32_t nextMs = 0;
    };

    struct Peer {
        std::uint32_t id = 0;
        Phase phase = Phase::kAskingService;
        std::uint32_t clockStartMs = 0; // the call, or the arrival of PeerCandidates
        OpId op = 0;                    // a ConnectToPeer waiting on this peer
        bool owned = false;             // a socket of the caller's talks to it
        bool queued = false;            // waiting in the accept queue
        bool releasing = false;
        bool incoming = false; // introduced by the other side
        // Asking the service.
        std::uint32_t connectTxn = 0;
        std::uint32_t askStartMs = 0;
        std::uint32_t lastAskMs = 0;
        bool waitingForRebind = false;
        // Punching.
        std::vector<std::uint32_t> punchIds;
        std::vector<ProbeTarget> targets;
        std::vector<platform::Endpoint> learned; // sent a valid probe or reply
        std::uint32_t answerUntilMs = 0;
        std::uint32_t lastRelayPingMs = 0;
        bool relayPinged = false;
        // The path.
        PathKind path = PathKind::kNone;
        platform::Endpoint direct;
        std::unique_ptr<PeerLink> link;
        platform::Error failure = platform::Error::kNone;
    };

    platform::Transport &transport_;
    RendezvousSettings settings_;
    platform::SocketId socket_;
    std::uint32_t nextTxn_;

    BindState bindState_ = BindState::kUnbound;
    platform::Error bindError_ = platform::Error::kNone;
    std::uint64_t session_ = 0;
    std::uint32_t self_ = 0;
    Ticket ticket_{};
    std::uint32_t bindTxn_ = 0;
    std::uint32_t bindStartMs_ = 0;
    std::uint32_t lastBindMs_ = 0;
    bool bindInFlight_ = false; // a Bind (first, refresh or rebind) waits for its answer
    std::uint32_t refreshMs_ = 15000;
    std::uint32_t lastRefreshMs_ = 0;

    std::map<std::uint32_t, Peer> peers_;
    std::map<OpId, OpStatus> ops_;
    OpId nextOp_ = 1;
    std::deque<std::uint32_t> accepted_;
    std::uint32_t acceptFailures_ = 0;
    std::uint32_t refused_ = 0;

    platform::Status openSocket();
    void sendBind(std::uint32_t nowMs);
    void sendTo(platform::Endpoint to, const std::vector<std::uint8_t> &bytes);
    void sendRv(platform::Endpoint to, const RvMessage &message);
    void sendConnect(Peer &peer, std::uint32_t nowMs);
    void flush(Peer &peer, std::vector<OutgoingEnvelope> &out);

    void onDatagram(platform::Endpoint from, const std::uint8_t *data, std::size_t bytes,
                    std::uint32_t nowMs);
    void onService(const RvMessage &message, std::uint32_t nowMs);
    void onBindAnswer(const RvMessage &message, std::uint32_t nowMs);
    void onCandidates(const RvMessage &message, std::uint32_t nowMs);
    void onProbe(platform::Endpoint from, const RvMessage &message, std::uint32_t nowMs);
    void onProbeReply(platform::Endpoint from, const RvMessage &message, std::uint32_t nowMs);
    void onEnvelope(platform::Endpoint from, const std::uint8_t *data, std::size_t bytes,
                    std::uint32_t nowMs);

    Peer &newPeer(std::uint32_t id, std::uint32_t nowMs);
    void startPunch(Peer &peer, const RvMessage &message, std::uint32_t nowMs);
    void addTarget(Peer &peer, platform::Endpoint endpoint, std::uint32_t firstMs);
    void choose(Peer &peer, PathKind path, platform::Endpoint direct, std::uint32_t nowMs);
    void fail(Peer &peer, platform::Error error);
    void bindFinished(std::uint32_t nowMs);
    void runPeer(Peer &peer, std::uint32_t nowMs);
    bool knowsAddress(const Peer &peer, platform::Endpoint from) const;
    const Peer *ownedPeer(std::uint32_t id) const;
};

} // namespace sfcop::net
