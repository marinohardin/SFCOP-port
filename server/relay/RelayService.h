// The UDP service on the rendezvous port (src/net/RENDEZVOUS.md): the table of bindings, the
// introductions of two peers, and the relay that forwards envelopes between bound peers. It is a
// state machine on three seams, so a test drives it with hand-written datagrams and a fake clock:
// a Sender for outgoing datagrams, a TimeSource, and a random source.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Log.h"
#include "Wire.h"

namespace sfcop::relay {

struct Endpoint {
    std::uint32_t address = 0;  // IPv4, host-order value
    std::uint16_t port = 0;
    bool operator==(const Endpoint& o) const { return address == o.address && port == o.port; }
    bool operator<(const Endpoint& o) const {
        return address != o.address ? address < o.address : port < o.port;
    }
};

class Sender {
public:
    virtual ~Sender() = default;
    virtual void send(const Endpoint& to, const std::uint8_t* data, std::size_t size) = 0;
};

class TimeSource {
public:
    virtual ~TimeSource() = default;
    virtual std::uint64_t nowMs() const = 0;         // monotonic
    virtual std::uint32_t unixSeconds() const = 0;   // for ticket expiry
};

// Every number is the document's unless a comment says otherwise.
struct RelayConfig {
    Bytes ticketKey;                               // 32 bytes, from the host's environment file
    std::size_t maxBindings = 8192;
    std::uint32_t bindingTimeoutMs = 45000;
    std::uint16_t refreshSeconds = 15;
    // Relay limits, per binding as the source: a token bucket with a one-second burst.
    std::uint32_t relayDatagramsPerSecond = 1000;
    std::uint32_t relayBytesPerSecond = 512 * 1024;
    // Rendezvous limits, per source address, a token bucket refilled evenly over the minute.
    std::uint32_t bindsPerMinute = 30;
    std::uint32_t connectsPerMinute = 60;
    std::uint32_t rendezvousPerMinute = 120;
    std::size_t maxTrackedSources = 65536;
    std::uint32_t introductionReuseMs = 3000;
};

struct RelayStats {
    std::uint64_t forwardedDatagrams = 0;
    std::uint64_t forwardedBytes = 0;
    std::uint64_t bindsAccepted = 0;
    std::uint64_t introductions = 0;
    std::uint64_t refusedUnknownKind = 0;     // neither SFRV nor SFNE, or too short for a magic
    std::uint64_t refusedMalformed = 0;       // a rendezvous message that does not decode
    std::uint64_t refusedBadEnvelope = 0;     // short, wrong version, or over 1200 bytes
    std::uint64_t refusedNotBound = 0;        // relay: source or destination has no binding
    std::uint64_t refusedBlocked = 0;         // source address on the owner's block list
    std::uint64_t rateLimitedRelay = 0;
    std::uint64_t rateLimitedRendezvous = 0;
    std::uint64_t rendezvousErrors = 0;       // RvError sent (bad ticket, full, unknown peer, ...)
};

class RelayService {
public:
    using Random = std::function<std::uint32_t()>;

    RelayService(RelayConfig config, Sender& sender, const TimeSource& time, const Logger& log,
                 Random random);

    void onDatagram(const Endpoint& from, const std::uint8_t* data, std::size_t size);
    // Expires bindings, sends the delayed second PeerCandidates, writes the minute's counts.
    void tick();
    // The lobby removed the session: drop every binding of it. Returns how many were dropped.
    std::size_t removeSession(std::uint64_t sessionId);
    void setBlocklist(std::unordered_set<std::uint32_t> addresses);

    std::size_t bindingCount() const { return bindingCount_.load(); }
    const RelayStats& stats() const { return stats_; }
    std::size_t trackedSources() const { return sources_.size(); }

private:
    struct Bucket {
        double tokens = 0;
        std::uint64_t lastMs = 0;
        bool primed = false;
    };
    struct Binding {
        Endpoint address;
        std::vector<Candidate> locals;
        std::uint64_t lastSeenMs = 0;
        Bucket datagrams;
        Bucket bytes;
    };
    struct SourceLimits {
        Bucket binds, connects, all;
        std::uint64_t lastSeenMs = 0;
        std::list<std::uint32_t>::iterator lruPosition;
    };
    struct Introduction {
        std::uint32_t punchId = 0;
        std::uint64_t atMs = 0;
    };
    struct Pending {
        std::uint64_t dueMs;
        Endpoint to;
        Bytes datagram;
    };
    using PeerMap = std::unordered_map<std::uint32_t, Binding>;

    void handleRendezvous(const Endpoint& from, const std::uint8_t* data, std::size_t size);
    void handleBind(const Endpoint& from, const BindMessage& m);
    void handleConnect(const Endpoint& from, const ConnectMessage& m);
    void handleUnbind(const Endpoint& from, const UnbindMessage& m);
    void handleEnvelope(const Endpoint& from, const std::uint8_t* data, std::size_t size);

    void sendError(const Endpoint& to, std::uint32_t transaction, std::uint32_t result);
    enum class Allow { kYes, kDropSilently, kReplyRateLimited };
    Allow takeSourceToken(const Endpoint& from, std::uint8_t type);
    static void refill(Bucket& b, std::uint64_t now, double ratePerSec, double capacity);
    Binding* find(std::uint64_t session, std::uint32_t peer);
    bool expired(const Binding& b) const;
    void dropBinding(PeerMap& peers, PeerMap::iterator it);
    bool validTicketMac(const Ticket& t) const;
    void logDebug(const std::string& s) const { log_.debug(s); }

    RelayConfig config_;
    Sender& sender_;
    const TimeSource& time_;
    const Logger& log_;
    Random random_;

    std::unordered_map<std::uint64_t, PeerMap> sessions_;
    // Keyed by IPv4 address alone (RENDEZVOUS.md: per source address); lru_ holds the addresses
    // most recently seen first, so a full table evicts the one idle the longest.
    std::unordered_map<std::uint32_t, SourceLimits> sources_;
    std::list<std::uint32_t> lru_;
    std::map<std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>, Introduction> introductions_;
    std::vector<Pending> pending_;
    std::unordered_set<std::uint32_t> blocklist_;
    std::size_t total_ = 0;
    std::atomic<std::size_t> bindingCount_{0};
    RelayStats stats_;
    RelayStats lastReported_;
    std::uint64_t lastMinuteMs_ = 0;
    bool minuteStarted_ = false;
};

}  // namespace sfcop::relay
