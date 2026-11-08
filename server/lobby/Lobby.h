// The lobby's session table and HTTP handlers (LOBBY_API.md). Everything is in memory; time and
// randomness are passed in so the tests are exact. `nowMs` is milliseconds since the Unix epoch.
#pragma once

#include "Http.h"
#include "Json.h"
#include "RateLimit.h"
#include "RelayClient.h"
#include "Ticket.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>

namespace sfcop::lobby {

constexpr int kApiVersion = 1;
constexpr int kProtocolVersion = 1;       // the envelope version served (PROTOCOL.md)
constexpr int kHeartbeatSeconds = 15;
constexpr int kSessionTtlSeconds = 45;
constexpr int kNextPollSeconds = 5;
constexpr size_t kMaxSessions = 2048;
constexpr size_t kMaxListed = 200;
constexpr size_t kRelayCallsPerSweep = 5;
constexpr size_t kRelayQueueMax = 4096;
constexpr int kTicketBudget = 64;          // per session per 10 minutes, renewals included
constexpr uint32_t kSessionPasswordRequired = 0x00000400; // DPSESSION_PASSWORDREQUIRED

struct LobbyConfig {
    std::string rendezvousHost = "localhost"; // ServiceAddress.host in the answers (the relay's name)
    uint16_t rendezvousPort = 47710;
    TicketKey ticketKey{};
    std::set<std::string> blocked; // source addresses answered with `forbidden` and nothing else
    size_t maxSessions = kMaxSessions;
};

using RandomFill = std::function<void(uint8_t* out, size_t size)>;

class Lobby {
public:
    Lobby(LobbyConfig config, RandomFill random, uint64_t startMs);

    // Never throws for a bad request; a defect inside answers `internal`.
    HttpResponse handle(const HttpRequest& request, uint64_t nowMs);

    // Removes sessions with no heartbeat for the time to live, and idle rate-limit state.
    void sweep(uint64_t nowMs);

    // Makes at most `maxCalls` of the queued removal calls to the relay and returns how many are
    // left. A request handler makes at most one and a sweep at most kRelayCallsPerSweep, so a
    // stalled relay costs a bounded time per turn of the HTTP loop; the rest wait for the next.
    size_t drainRelay(size_t maxCalls);

    bool hasSession(uint64_t sessionId) const { return sessions_.count(sessionId) != 0; }
    size_t sessionCount() const { return sessions_.size(); }

    // Told of every session that leaves the table (closed or expired), so the relay drops its
    // bindings. Not owned; may be null.
    void setRelayClient(RelayClient* relay) { relay_ = relay; }

    // Reconfigures the owner's block list without a restart.
    void setBlocked(std::set<std::string> blocked) { config_.blocked = std::move(blocked); }

private:
    struct Session {
        uint64_t id = 0;
        int protocol = 0;
        std::string application; // upper case, braces included
        std::string hostSecret;  // base64url
        std::string name;
        uint32_t flags = 0;
        int maxPlayers = 0;
        int currentPlayers = 0;
        uint32_t user[4] = {0, 0, 0, 0};
        bool passwordRequired = false;
        uint64_t createdMs = 0;
        uint64_t lastBeatMs = 0;
        uint32_t nextPeer = 2;
        std::deque<uint64_t> ticketTimes; // issue times within the last 10 minutes
    };

    struct Description {
        std::string name;
        uint32_t flags = 0;
        int maxPlayers = 0;
        int currentPlayers = 0;
        uint32_t user[4] = {0, 0, 0, 0};
        bool passwordRequired = false;
    };

    HttpResponse route(const HttpRequest& req, uint64_t nowMs, std::string& pattern);
    HttpResponse doHealth(uint64_t nowMs);
    HttpResponse doList(const HttpRequest& req, uint64_t nowMs);
    HttpResponse doRegister(const HttpRequest& req, uint64_t nowMs);
    HttpResponse doHeartbeat(const HttpRequest& req, uint64_t sessionId, uint64_t nowMs);
    HttpResponse doDelete(const HttpRequest& req, uint64_t sessionId, uint64_t nowMs);
    HttpResponse doTicket(const HttpRequest& req, uint64_t sessionId, uint64_t nowMs);

    bool limited(const HttpRequest& req, int kind, int perMinute, uint64_t nowMs, HttpResponse& out);
    void removeSession(uint64_t id, const char* why);
    std::string newSecret();
    uint64_t newSessionId();
    Json sessionInfo(const Session& s, uint64_t nowMs) const;
    Json rendezvousAddress() const;
    std::string issueTicket(uint64_t session, uint32_t peer, uint8_t role, uint64_t nowMs) const;

    LobbyConfig config_;
    RandomFill random_;
    uint64_t startMs_;
    std::map<uint64_t, Session> sessions_;
    RateLimiter limiter_;
    RelayClient* relay_ = nullptr;
    std::deque<uint64_t> relayQueue_;
};

// The per-request log line: method, route pattern (the session id left out), status and duration.
// Never the body, the source address or a secret (LOBBY_API.md, "What is logged").
std::string requestLogLine(const std::string& method, const HttpResponse& response, uint64_t durationMs);

std::string hexId(uint64_t value, int digits);
bool parseHexId(const std::string& text, int digits, uint64_t& out);
// "{0AB1C531-4745-11D1-A7A1-0000F803ABFC}" in either case; the upper-case form, or empty.
std::string normalizeGuid(const std::string& text);

} // namespace sfcop::lobby
