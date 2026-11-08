// A stand-in for the rendezvous and relay service of src/net/RENDEZVOUS.md, and for the one part
// of the lobby the client needs, issuing tickets (LOBBY_API.md). Test code only: N3 and N4 build
// the real services. It speaks the documents' messages byte for byte, checks tickets with
// HMAC-SHA-256 as the service does, introduces peers with two copies of PeerCandidates 100 ms
// apart, and relays envelopes between bound peers of one session. Rate limits and the health
// endpoint are left out.
//
// It does no I/O and reads no clock: the caller hands it datagrams with the time and sends what
// it returns, so the in-process tests run it on simulated time and the two-process harness on
// real sockets.
#pragma once

#include "app/Sha256.h"
#include "net/Envelope.h"
#include "net/RendezvousMessages.h"
#include "platform/Transport/Transport.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace sfcop::nettest {

using ServiceKey = std::array<std::uint8_t, 32>;

struct ServiceDatagram {
    platform::Endpoint to;
    std::vector<std::uint8_t> bytes;
};

inline app::Sha256::Digest hmacSha256(const ServiceKey &key, const std::uint8_t *data,
                                      std::size_t bytes) {
    std::array<std::uint8_t, 64> inner{};
    std::array<std::uint8_t, 64> outer{};
    for (std::size_t i = 0; i < 64; ++i) {
        std::uint8_t k = i < key.size() ? key[i] : 0;
        inner[i] = k ^ 0x36;
        outer[i] = k ^ 0x5C;
    }
    app::Sha256 first;
    first.update(inner.data(), inner.size());
    first.update(data, bytes);
    app::Sha256::Digest innerDigest = first.finish();
    app::Sha256 second;
    second.update(outer.data(), outer.size());
    second.update(innerDigest.data(), innerDigest.size());
    return second.finish();
}

// The lobby's ticket for `peer` of `session` (RENDEZVOUS.md, "Ticket").
inline net::Ticket issueTicket(const ServiceKey &key, std::uint64_t session, std::uint32_t peer,
                               std::uint32_t expiresAt, std::uint8_t role) {
    net::TicketFields fields;
    fields.sessionId = session;
    fields.peerId = peer;
    fields.expiresAt = expiresAt;
    fields.role = role;
    net::Ticket ticket = net::writeTicket(fields);
    app::Sha256::Digest mac = hmacSha256(key, ticket.data(), 20);
    for (std::size_t i = 0; i < 16; ++i) {
        ticket[20 + i] = mac[i];
    }
    return ticket;
}

class StandInService {
public:
    static constexpr std::size_t kMaxBindings = 8192;
    static constexpr std::uint32_t kBindingLifetimeMs = 45000;
    static constexpr std::uint32_t kSecondCopyMs = 100;

    explicit StandInService(ServiceKey key, std::uint32_t seed = 0x2545F491)
        : key_(key), random_(seed == 0 ? 1 : seed) {}

    // One datagram that arrived at the service port. `unixSeconds` is the time tickets are
    // checked against.
    void OnDatagram(platform::Endpoint from, const std::uint8_t *data, std::size_t bytes,
                    std::uint32_t nowMs, std::uint32_t unixSeconds,
                    std::vector<ServiceDatagram> &out) {
        std::uint32_t magic = bytes >= 4 ? static_cast<std::uint32_t>(data[0]) |
                                               (static_cast<std::uint32_t>(data[1]) << 8) |
                                               (static_cast<std::uint32_t>(data[2]) << 16) |
                                               (static_cast<std::uint32_t>(data[3]) << 24)
                                         : 0;
        if (magic == net::kEnvelopeMagic) {
            relay(from, data, bytes, out);
            return;
        }
        if (magic != net::kRendezvousMagic) {
            ++refused_;
            return;
        }
        std::optional<std::uint8_t> version = net::rendezvousVersion(data, bytes);
        if (version && *version != net::kRendezvousVersion && bytes >= 12) {
            std::uint32_t transaction = static_cast<std::uint32_t>(data[8]) |
                                        (static_cast<std::uint32_t>(data[9]) << 8) |
                                        (static_cast<std::uint32_t>(data[10]) << 16) |
                                        (static_cast<std::uint32_t>(data[11]) << 24);
            sendError(from, transaction, net::RvResult::kUnsupportedVersion, out);
            return;
        }
        std::optional<net::RvMessage> message = net::decodeRendezvous(data, bytes);
        if (!message) {
            ++refused_;
            return;
        }
        switch (message->type) {
        case net::RvType::kBind:
            bind(from, *message, nowMs, unixSeconds, out);
            return;
        case net::RvType::kConnect:
            connect(from, *message, nowMs, out);
            return;
        case net::RvType::kUnbind: {
            auto found = bindings_.find({message->sessionId, message->peer});
            if (found != bindings_.end() && found->second.address == from) {
                bindings_.erase(found);
            }
            return;
        }
        default:
            ++refused_; // the peers' own messages never come here
            return;
        }
    }

    // Sends the second copies that are due and forgets bindings not refreshed for 45 s.
    void Tick(std::uint32_t nowMs, std::vector<ServiceDatagram> &out) {
        for (auto it = copies_.begin(); it != copies_.end();) {
            if (static_cast<std::int32_t>(nowMs - it->first) >= 0) {
                out.push_back(it->second);
                it = copies_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = bindings_.begin(); it != bindings_.end();) {
            it = nowMs - it->second.refreshedMs >= kBindingLifetimeMs ? bindings_.erase(it)
                                                                      : std::next(it);
        }
    }

    // The lobby removed the session: so are its bindings (RENDEZVOUS.md, "Bindings").
    void RemoveSession(std::uint64_t session) {
        for (auto it = bindings_.begin(); it != bindings_.end();) {
            it = it->first.first == session ? bindings_.erase(it) : std::next(it);
        }
    }

    // Test switches: drop every relayed envelope, or forget every binding (a restart).
    void SetRelayEnabled(bool enabled) { relayEnabled_ = enabled; }
    void Restart() { bindings_.clear(); }

    std::size_t Bindings() const { return bindings_.size(); }
    std::uint32_t Relayed() const { return relayed_; }
    std::uint32_t Refused() const { return refused_; }
    std::uint32_t Introductions() const { return introductions_; }

private:
    struct Binding {
        platform::Endpoint address;
        std::vector<net::Candidate> locals;
        std::uint32_t refreshedMs = 0;
    };

    using Key = std::pair<std::uint64_t, std::uint32_t>;

    ServiceKey key_;
    std::uint32_t random_;
    std::map<Key, Binding> bindings_;
    std::multimap<std::uint32_t, ServiceDatagram> copies_;
    bool relayEnabled_ = true;
    std::uint32_t relayed_ = 0;
    std::uint32_t refused_ = 0;
    std::uint32_t introductions_ = 0;

    std::uint32_t nextRandom() {
        // xorshift32; never 0
        random_ ^= random_ << 13;
        random_ ^= random_ >> 17;
        random_ ^= random_ << 5;
        return random_;
    }

    static void send(platform::Endpoint to, const net::RvMessage &message,
                     std::vector<ServiceDatagram> &out) {
        out.push_back(ServiceDatagram{to, net::encodeRendezvous(message)});
    }

    static void sendError(platform::Endpoint to, std::uint32_t transaction, net::RvResult result,
                          std::vector<ServiceDatagram> &out) {
        net::RvMessage error;
        error.type = net::RvType::kError;
        error.transaction = transaction;
        error.result = result;
        send(to, error, out);
    }

    bool sessionExists(std::uint64_t session) const {
        auto found = bindings_.lower_bound({session, 0});
        return found != bindings_.end() && found->first.first == session;
    }

    void bind(platform::Endpoint from, const net::RvMessage &message, std::uint32_t nowMs,
              std::uint32_t unixSeconds, std::vector<ServiceDatagram> &out) {
        net::TicketFields ticket = net::readTicket(message.ticket);
        app::Sha256::Digest mac = hmacSha256(key_, message.ticket.data(), 20);
        for (std::size_t i = 0; i < 16; ++i) {
            if (mac[i] != ticket.mac[i]) {
                sendError(from, message.transaction, net::RvResult::kBadTicket, out);
                return;
            }
        }
        const bool expired = ticket.expiresAt <= unixSeconds;
        Key key{ticket.sessionId, ticket.peerId};
        auto found = bindings_.find(key);
        if (found == bindings_.end() || found->second.address != from) {
            // A new binding, or a changed address: only with an unexpired ticket.
            if (expired) {
                sendError(from, message.transaction, net::RvResult::kTicketExpired, out);
                return;
            }
            if (found == bindings_.end() && bindings_.size() >= kMaxBindings) {
                sendError(from, message.transaction, net::RvResult::kFull, out);
                return;
            }
        }
        Binding &binding = bindings_[key];
        binding.address = from;
        binding.locals = message.candidates;
        binding.refreshedMs = nowMs;
        net::RvMessage reply;
        reply.type = net::RvType::kBindReply;
        reply.transaction = message.transaction;
        reply.result = net::RvResult::kOk;
        reply.reflexive = net::Candidate{from.address, from.port, net::CandidateKind::kReflexive};
        reply.refreshSeconds = 15;
        send(from, reply, out);
    }

    std::vector<net::Candidate> candidatesOf(const Binding &binding) const {
        std::vector<net::Candidate> list;
        list.push_back(net::Candidate{binding.address.address, binding.address.port,
                                      net::CandidateKind::kReflexive});
        for (const net::Candidate &local : binding.locals) {
            if (list.size() < net::kMaxPeerCandidates) {
                list.push_back(local);
            }
        }
        return list;
    }

    void connect(platform::Endpoint from, const net::RvMessage &message, std::uint32_t nowMs,
                 std::vector<ServiceDatagram> &out) {
        auto source = bindings_.find({message.sessionId, message.source});
        if (source == bindings_.end() || source->second.address != from) {
            sendError(from, message.transaction,
                      sessionExists(message.sessionId) ? net::RvResult::kUnknownBinding
                                                       : net::RvResult::kUnknownSession,
                      out);
            return;
        }
        auto target = bindings_.find({message.sessionId, message.target});
        if (target == bindings_.end() || message.target == message.source) {
            sendError(from, message.transaction, net::RvResult::kUnknownPeer, out);
            return;
        }
        ++introductions_;
        const std::uint32_t punchId = nextRandom();
        net::RvMessage toSource;
        toSource.type = net::RvType::kPeerCandidates;
        toSource.transaction = message.transaction;
        toSource.sessionId = message.sessionId;
        toSource.peer = message.target;
        toSource.punchId = punchId;
        toSource.candidates = candidatesOf(target->second);
        net::RvMessage toTarget = toSource;
        toTarget.transaction = 0;
        toTarget.peer = message.source;
        toTarget.candidates = candidatesOf(source->second);
        ServiceDatagram a{source->second.address, net::encodeRendezvous(toSource)};
        ServiceDatagram b{target->second.address, net::encodeRendezvous(toTarget)};
        out.push_back(a);
        out.push_back(b);
        copies_.emplace(nowMs + kSecondCopyMs, a);
        copies_.emplace(nowMs + kSecondCopyMs, b);
    }

    void relay(platform::Endpoint from, const std::uint8_t *data, std::size_t bytes,
               std::vector<ServiceDatagram> &out) {
        std::optional<net::EnvelopeHeader> header = net::peekEnvelopeHeader(data, bytes);
        if (!header || !relayEnabled_) {
            ++refused_;
            return;
        }
        auto source = bindings_.find({header->sessionId, header->source});
        auto destination = bindings_.find({header->sessionId, header->destination});
        if (source == bindings_.end() || source->second.address != from ||
            destination == bindings_.end()) {
            ++refused_; // no answer: nobody learns which sessions exist
            return;
        }
        ++relayed_;
        out.push_back(ServiceDatagram{destination->second.address,
                                      std::vector<std::uint8_t>(data, data + bytes)});
    }
};

} // namespace sfcop::nettest
