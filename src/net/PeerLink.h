// The envelope's rules for one path between two machines (PROTOCOL.md, "Envelope", "Rules"):
// sequence numbers and the duplicate window, fragmentation and reassembly, reliable delivery with
// acks and retransmission, keepalive pings, liveness and closing. It knows nothing of sockets or
// addresses: what it sends comes out as envelopes tagged with the route to send them on, and the
// caller (RendezvousEngine) puts them on the wire. Time is passed in, in milliseconds of a clock
// that may wrap.
//
// Not thread-safe; its owner serialises calls.
#pragma once

#include "net/Envelope.h"
#include "platform/Core/Result.h"

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

namespace sfcop::net {

// Where an envelope goes: the path the link chose, or a named one (a pong goes back on the path
// its ping came in on; a relay open pings the relay before it is chosen).
enum class Route : std::uint8_t { kChosen, kDirect, kRelay };

struct OutgoingEnvelope {
    Route route = Route::kChosen;
    std::vector<std::uint8_t> bytes;
};

// The largest message one Send() takes: the largest UDP datagram, as Transport::Send() allows.
constexpr std::size_t kMaxLinkMessage = 65507;

class PeerLink {
public:
    PeerLink(std::uint64_t sessionId, std::uint32_t self, std::uint32_t peer, std::uint32_t nowMs);

    // Sends one message in EnvelopeData, split into fragments of 1168 bytes. Errors:
    // kInvalidArgument for an empty message or one over kMaxLinkMessage; kWouldBlock when
    // `reliable` and 256 reliable messages are unacknowledged; kConnectionReset once the link is
    // dead or closed.
    platform::Status Send(const std::uint8_t *data, std::size_t bytes, bool reliable,
                          std::uint32_t nowMs, std::vector<OutgoingEnvelope> &out);

    // Sends EnvelopeClose (reliable) with `reason`; the link is closed for sending afterwards.
    void Close(std::uint32_t reason, std::uint32_t nowMs, std::vector<OutgoingEnvelope> &out);

    // Sends an EnvelopePing on `route` and returns its nonce.
    std::uint32_t Ping(Route route, std::uint32_t nowMs, std::vector<OutgoingEnvelope> &out);

    // Takes one envelope that arrived on `arrivedOn`. An envelope for another session, another
    // destination or from another source is ignored and returns false; so is a duplicate.
    bool Receive(const Envelope &envelope, Route arrivedOn, std::uint32_t nowMs,
                 std::vector<OutgoingEnvelope> &out);

    // Runs the timers: acks due, retransmissions, reassembly give-up, keepalive.
    void Tick(std::uint32_t nowMs, std::vector<OutgoingEnvelope> &out);

    bool HasMessage() const { return !messages_.empty(); }
    std::size_t NextMessageBytes() const {
        return messages_.empty() ? 0 : messages_.front().size();
    }
    std::vector<std::uint8_t> TakeMessage();

    // Nothing arrived for 10 s, or a reliable message went 10 s without an ack.
    bool Dead(std::uint32_t nowMs) const;
    bool PeerClosed() const { return peerClosed_; }
    // True once the link sent its Close and the Close was acknowledged (or the link died).
    bool CloseFinished(std::uint32_t nowMs) const;
    // A pong arrived on this route since the link was made.
    bool PongSeen(Route route) const { return route == Route::kRelay ? pongRelay_ : pongDirect_; }
    std::uint32_t SmoothedRttMs() const { return srttMs_; }
    std::uint32_t Peer() const { return peer_; }

private:
    // The duplicate window of one counter: the highest number taken and the 1024 below it.
    class Window {
    public:
        bool Fresh(std::uint32_t sequence) const;
        void Mark(std::uint32_t sequence);

    private:
        bool any_ = false;
        std::uint32_t highest_ = 0;
        std::bitset<1024> seen_; // bit i: highest_ - i was taken
    };

    struct Partial {
        std::uint16_t count = 0;
        std::uint16_t received = 0;
        std::size_t bytes = 0;
        std::uint32_t firstMs = 0;
        std::vector<std::vector<std::uint8_t>> fragments;
    };

    struct Unacked {
        std::vector<std::vector<std::uint8_t>> envelopes;
        std::uint32_t firstSentMs = 0;
        std::uint32_t lastSentMs = 0;
        std::uint32_t timeoutMs = 0;
    };

    // A reliable message taken out of order, held until the gap before it closes.
    struct Held {
        EnvelopeKind kind = EnvelopeKind::kData;
        std::vector<std::uint8_t> payload;
    };

    std::uint64_t session_;
    std::uint32_t self_;
    std::uint32_t peer_;

    std::uint32_t nextUnreliable_ = 1;
    std::uint32_t nextReliable_ = 1;
    std::map<std::uint32_t, Unacked> unacked_;
    bool closing_ = false;

    Window window_;
    std::uint32_t nextExpected_ = 1; // the next reliable number to deliver
    std::map<std::uint32_t, Held> held_;
    std::size_t heldBytes_ = 0;
    std::map<std::uint64_t, Partial> partials_;
    std::size_t partialBytes_ = 0;
    bool ackDue_ = false;
    std::uint32_t ackDueMs_ = 0;

    std::deque<std::vector<std::uint8_t>> messages_;
    bool peerClosed_ = false;
    bool pongDirect_ = false;
    bool pongRelay_ = false;
    std::uint32_t nextNonce_ = 1;
    std::uint32_t srttMs_ = 0;

    std::uint32_t lastSentMs_;
    std::uint32_t lastReceivedMs_;
    bool reliableDead_ = false;

    EnvelopeHeader header(EnvelopeKind kind, bool reliable, std::uint32_t sequence) const;
    void emit(Route route, const Envelope &envelope, std::uint32_t nowMs,
              std::vector<OutgoingEnvelope> &out);
    std::uint32_t retransmitTimeout() const;
    void sendAck(std::uint32_t nowMs, std::vector<OutgoingEnvelope> &out);
    void takeAck(const Envelope &envelope);
    // A whole message (reassembled or single) of sequence `sequence`.
    void deliver(EnvelopeKind kind, bool reliable, std::uint32_t sequence,
                 std::vector<std::uint8_t> payload, std::uint32_t nowMs,
                 std::vector<OutgoingEnvelope> &out);
    void accept(EnvelopeKind kind, std::vector<std::uint8_t> payload);
    bool reassemble(const Envelope &envelope, bool reliable, std::uint32_t nowMs,
                    std::vector<std::uint8_t> &whole);
};

} // namespace sfcop::net
