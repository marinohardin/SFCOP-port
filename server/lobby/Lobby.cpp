#include "Lobby.h"

#include "Base64Url.h"
#include "Log.h"
#include "Sha256.h"

#include <algorithm>
#include <exception>

namespace sfcop::lobby {

namespace {

constexpr const char* kTag = "lobby";

// Rate-limit buckets (LOBBY_API.md, "Rate limits").
enum Kind { kAll = 0, kList, kRegister, kHeartbeat, kDelete, kTicket, kHealth, kEventLog };

HttpResponse errorResponse(int status, const char* code, const std::string& message, int retryAfter = 0) {
    Json body = Json::makeObject();
    Json err = Json::makeObject();
    err.set("code", Json::makeString(code));
    err.set("message", Json::makeString(message));
    if (retryAfter > 0) err.set("retry_after_s", Json::makeInt(retryAfter));
    body.set("error", std::move(err));
    HttpResponse r;
    r.status = status;
    r.body = body.dump();
    if (retryAfter > 0) r.headers["Retry-After"] = std::to_string(retryAfter);
    return r;
}

HttpResponse badRequest(const std::string& message) { return errorResponse(400, "bad_request", message); }
HttpResponse unsupportedVersion() {
    return errorResponse(400, "unsupported_version", "The protocol version is not served by this lobby.");
}
HttpResponse forbidden(const std::string& message) { return errorResponse(403, "forbidden", message); }
HttpResponse notFound() { return errorResponse(404, "not_found", "No such session or path."); }

HttpResponse jsonResponse(int status, const Json& body) {
    HttpResponse r;
    r.status = status;
    r.body = body.dump();
    return r;
}

// A required integer field within [lo, hi], or the message that says why not.
bool intField(const Json& obj, const char* name, int64_t lo, int64_t hi, int64_t& out, std::string& why) {
    const Json* f = obj.find(name);
    if (!f) {
        why = std::string("Missing field ") + name + ".";
        return false;
    }
    if (f->kind != Json::Kind::Integer || f->integer < lo || f->integer > hi) {
        why = std::string("Field ") + name + " is not an integer in range.";
        return false;
    }
    out = f->integer;
    return true;
}

bool stringField(const Json& obj, const char* name, std::string& out, std::string& why) {
    const Json* f = obj.find(name);
    if (!f) {
        why = std::string("Missing field ") + name + ".";
        return false;
    }
    if (f->kind != Json::Kind::String) {
        why = std::string("Field ") + name + " is not a string.";
        return false;
    }
    out = f->string;
    return true;
}

bool isHexDigitLower(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

std::map<std::string, std::string> parseQuery(const std::string& query, bool& ok) {
    std::map<std::string, std::string> out;
    ok = true;
    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        std::string pair = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        pos = amp == std::string::npos ? query.size() : amp + 1;
        if (pair.empty()) continue;
        size_t eq = pair.find('=');
        std::string k, v;
        if (!percentDecode(pair.substr(0, eq), k) ||
            (eq != std::string::npos && !percentDecode(pair.substr(eq + 1), v))) {
            ok = false;
            return out;
        }
        out[k] = v;
    }
    return out;
}

bool isLoopback(const std::string& source) { return source == "127.0.0.1" || source == "::1"; }

} // namespace

std::string hexId(uint64_t value, int digits) {
    static const char* d = "0123456789abcdef";
    std::string s(size_t(digits), '0');
    for (int i = digits - 1; i >= 0; --i) {
        s[size_t(i)] = d[value & 15];
        value >>= 4;
    }
    return s;
}

bool parseHexId(const std::string& text, int digits, uint64_t& out) {
    if (text.size() != size_t(digits)) return false;
    uint64_t v = 0;
    for (char c : text) {
        if (!isHexDigitLower(c)) return false;
        v = (v << 4) | uint64_t(c <= '9' ? c - '0' : c - 'a' + 10);
    }
    out = v;
    return true;
}

std::string normalizeGuid(const std::string& text) {
    if (text.size() != 38 || text.front() != '{' || text.back() != '}') return "";
    std::string out = text;
    for (size_t i = 1; i + 1 < out.size(); ++i) {
        bool dash = (i == 9 || i == 14 || i == 19 || i == 24);
        char c = out[i];
        if (dash) {
            if (c != '-') return "";
        } else if (c >= 'a' && c <= 'f') {
            out[i] = char(c - 'a' + 'A');
        } else if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) {
            return "";
        }
    }
    return out;
}

std::string requestLogLine(const std::string& method, const HttpResponse& response, uint64_t durationMs) {
    return method + " " + (response.pattern.empty() ? "(unrouted)" : response.pattern) + " " +
           std::to_string(response.status) + " " + std::to_string(durationMs) + "ms";
}

Lobby::Lobby(LobbyConfig config, RandomFill random, uint64_t startMs)
    : config_(std::move(config)), random_(std::move(random)), startMs_(startMs) {}

std::string Lobby::newSecret() {
    uint8_t bytes[16];
    random_(bytes, sizeof bytes);
    return base64UrlEncode(bytes, sizeof bytes);
}

uint64_t Lobby::newSessionId() {
    while (true) {
        uint8_t bytes[8];
        random_(bytes, sizeof bytes);
        uint64_t id = 0;
        for (int i = 0; i < 8; ++i) id = (id << 8) | bytes[i];
        if (id != 0 && !sessions_.count(id)) return id;
    }
}

std::string Lobby::issueTicket(uint64_t session, uint32_t peer, uint8_t role, uint64_t nowMs) const {
    TicketBody body;
    body.sessionId = session;
    body.peerId = peer;
    body.expiresAt = uint32_t(nowMs / 1000 + kTicketLifetimeSeconds);
    body.role = role;
    TicketBytes t = makeTicket(config_.ticketKey, body);
    return base64UrlEncode(t.data(), t.size());
}

Json Lobby::rendezvousAddress() const {
    Json a = Json::makeObject();
    a.set("host", Json::makeString(config_.rendezvousHost));
    a.set("port", Json::makeInt(config_.rendezvousPort));
    return a;
}

Json Lobby::sessionInfo(const Session& s, uint64_t nowMs) const {
    Json j = Json::makeObject();
    j.set("session_id", Json::makeString(hexId(s.id, 16)));
    j.set("protocol", Json::makeInt(s.protocol));
    j.set("application", Json::makeString(s.application));
    j.set("name", Json::makeString(s.name));
    j.set("flags", Json::makeInt(int64_t(s.flags | (s.passwordRequired ? kSessionPasswordRequired : 0))));
    j.set("max_players", Json::makeInt(s.maxPlayers));
    j.set("current_players", Json::makeInt(s.currentPlayers));
    Json user = Json::makeArray();
    for (uint32_t w : s.user) user.push(Json::makeInt(w));
    j.set("user", std::move(user));
    j.set("age_s", Json::makeInt(int64_t((nowMs - s.createdMs) / 1000)));
    return j;
}

bool Lobby::limited(const HttpRequest& req, int kind, int perMinute, uint64_t nowMs, HttpResponse& out) {
    int retry = 0;
    if (limiter_.allow(req.source, kind, perMinute, nowMs, &retry)) return false;
    int ignored = 0;
    if (limiter_.allow(req.source, kEventLog, 1, nowMs, &ignored)) {
        log::warn(kTag, "rate limit reached by " + req.source);
    }
    out = errorResponse(429, "rate_limited", "Too many requests; slow down.", retry);
    return true;
}

void Lobby::removeSession(uint64_t id, const char* why) {
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return;
    log::info(kTag, "session " + hexId(id, 16) + " removed (" + why + "), " +
                        std::to_string(it->second.currentPlayers) + " players");
    sessions_.erase(it);
    relayQueue_.push_back(id);
    if (relayQueue_.size() > kRelayQueueMax) relayQueue_.pop_front(); // the relay's bindings lapse anyway
}

size_t Lobby::drainRelay(size_t maxCalls) {
    while (relay_ && maxCalls > 0 && !relayQueue_.empty()) {
        uint64_t id = relayQueue_.front();
        relayQueue_.pop_front();
        --maxCalls;
        relay_->removeSession(id);
    }
    return relayQueue_.size();
}

void Lobby::sweep(uint64_t nowMs) {
    std::vector<uint64_t> dead;
    for (const auto& kv : sessions_) {
        if (nowMs >= kv.second.lastBeatMs + uint64_t(kSessionTtlSeconds) * 1000) dead.push_back(kv.first);
    }
    for (uint64_t id : dead) removeSession(id, "no heartbeat");
    limiter_.sweep(nowMs);
    drainRelay(kRelayCallsPerSweep);
}

HttpResponse Lobby::handle(const HttpRequest& request, uint64_t nowMs) {
    std::string pattern = "(unrouted)";
    HttpResponse response;
    try {
        if (config_.blocked.count(request.source)) {
            int ignored = 0;
            if (limiter_.allow(request.source, kEventLog, 1, nowMs, &ignored)) {
                log::warn(kTag, "blocked address " + request.source + " refused");
            }
            response = forbidden("This address is blocked.");
            pattern = "(blocked)";
        } else {
            response = route(request, nowMs, pattern);
        }
    } catch (const std::exception& e) {
        log::error(kTag, std::string("internal error: ") + e.what());
        response = errorResponse(500, "internal", "A defect in the lobby.");
    }
    response.pattern = pattern;
    drainRelay(1);
    return response;
}

HttpResponse Lobby::route(const HttpRequest& req, uint64_t nowMs, std::string& pattern) {
    HttpResponse limitedResponse;
    const std::string& p = req.path;

    if (p == "/v1/health") {
        pattern = "/v1/health";
        if (req.method != "GET") return notFound();
        if (!isLoopback(req.source) && limited(req, kHealth, 6, nowMs, limitedResponse)) return limitedResponse;
        return doHealth(nowMs);
    }

    // Everything else counts against the all-requests bucket first.
    if (limited(req, kAll, 120, nowMs, limitedResponse)) return limitedResponse;

    if (p == "/v1/sessions") {
        pattern = "/v1/sessions";
        if (req.method == "GET") {
            if (limited(req, kList, 30, nowMs, limitedResponse)) return limitedResponse;
            return doList(req, nowMs);
        }
        if (req.method == "POST") {
            if (limited(req, kRegister, 6, nowMs, limitedResponse)) return limitedResponse;
            return doRegister(req, nowMs);
        }
        return notFound();
    }

    const std::string prefix = "/v1/sessions/";
    if (p.compare(0, prefix.size(), prefix) == 0) {
        std::string rest = p.substr(prefix.size());
        std::string idText = rest;
        bool tickets = false;
        size_t slash = rest.find('/');
        if (slash != std::string::npos) {
            if (rest.substr(slash) != "/tickets") {
                pattern = "(unrouted)";
                return notFound();
            }
            idText = rest.substr(0, slash);
            tickets = true;
        }
        pattern = tickets ? "/v1/sessions/<session>/tickets" : "/v1/sessions/<session>";
        if (idText.empty()) return notFound();
        int kind = -1;
        int perMinute = 0;
        if (tickets && req.method == "POST") {
            kind = kTicket;
            perMinute = 30;
        } else if (!tickets && req.method == "PUT") {
            kind = kHeartbeat;
            perMinute = 10;
        } else if (!tickets && req.method == "DELETE") {
            kind = kDelete;
            perMinute = 6;
        } else {
            return notFound();
        }
        if (limited(req, kind, perMinute, nowMs, limitedResponse)) return limitedResponse;
        uint64_t id = 0;
        if (!parseHexId(idText, 16, id)) return badRequest("The session id is not 16 lowercase hex digits.");
        if (tickets) return doTicket(req, id, nowMs);
        return kind == kHeartbeat ? doHeartbeat(req, id, nowMs) : doDelete(req, id, nowMs);
    }

    pattern = "(unrouted)";
    return notFound();
}

HttpResponse Lobby::doHealth(uint64_t nowMs) {
    Json j = Json::makeObject();
    j.set("status", Json::makeString("ok"));
    j.set("api", Json::makeInt(kApiVersion));
    Json protocols = Json::makeArray();
    protocols.push(Json::makeInt(kProtocolVersion));
    j.set("protocols", std::move(protocols));
    size_t live = 0;
    for (const auto& kv : sessions_) {
        if (nowMs < kv.second.lastBeatMs + uint64_t(kSessionTtlSeconds) * 1000) ++live;
    }
    j.set("sessions", Json::makeInt(int64_t(live)));
    j.set("uptime_s", Json::makeInt(int64_t(nowMs >= startMs_ ? (nowMs - startMs_) / 1000 : 0)));
    return jsonResponse(200, j);
}

HttpResponse Lobby::doList(const HttpRequest& req, uint64_t nowMs) {
    bool ok = true;
    auto q = parseQuery(req.query, ok);
    if (!ok) return badRequest("The query has a bad escape.");
    auto pit = q.find("protocol");
    auto ait = q.find("application");
    if (pit == q.end() || ait == q.end()) return badRequest("The query needs protocol and application.");
    if (pit->second.empty() || pit->second.size() > 9 ||
        !std::all_of(pit->second.begin(), pit->second.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return badRequest("The protocol is not a number.");
    }
    std::string app = normalizeGuid(ait->second);
    if (app.empty()) return badRequest("The application is not a GUID.");
    int protocol = std::atoi(pit->second.c_str());
    if (protocol != kProtocolVersion) return unsupportedVersion();

    std::vector<const Session*> hits;
    for (const auto& kv : sessions_) {
        const Session& s = kv.second;
        if (nowMs >= s.lastBeatMs + uint64_t(kSessionTtlSeconds) * 1000) continue;
        if (s.protocol == protocol && s.application == app) hits.push_back(&s);
    }
    std::sort(hits.begin(), hits.end(), [](const Session* a, const Session* b) {
        if (a->createdMs != b->createdMs) return a->createdMs > b->createdMs; // newest first
        return a->id > b->id;
    });
    if (hits.size() > kMaxListed) hits.resize(kMaxListed);
    Json list = Json::makeArray();
    for (const Session* s : hits) list.push(sessionInfo(*s, nowMs));
    Json j = Json::makeObject();
    j.set("sessions", std::move(list));
    j.set("next_poll_s", Json::makeInt(kNextPollSeconds));
    return jsonResponse(200, j);
}

namespace {

// Reads protocol/name/flags/max_players/current_players/user/password_required; the message of the
// first problem, or empty.
std::string readDescription(const Json& body, std::string& name, uint32_t& flags, int& maxPlayers,
                            int& currentPlayers, uint32_t user[4], bool& passwordRequired) {
    std::string why;
    int64_t v = 0;
    if (!stringField(body, "name", name, why)) return why;
    if (name.empty() || name.size() > 63) return "The name must be 1 to 63 bytes.";
    if (!isValidUtf8(name)) return "The name is not UTF-8.";
    if (!intField(body, "flags", 0, 0xFFFFFFFFll, v, why)) return why;
    flags = uint32_t(v);
    if (!intField(body, "max_players", 0, 64, v, why)) return why;
    maxPlayers = int(v);
    if (!intField(body, "current_players", 0, maxPlayers == 0 ? 64 : maxPlayers, v, why)) return why;
    currentPlayers = int(v);
    const Json* u = body.find("user");
    if (!u || u->kind != Json::Kind::Array || u->array.size() != 4) return "Field user must be four integers.";
    for (size_t i = 0; i < 4; ++i) {
        const Json& w = u->array[i];
        if (w.kind != Json::Kind::Integer || w.integer < 0 || w.integer > 0xFFFFFFFFll) {
            return "Field user must be four unsigned 32-bit integers.";
        }
        user[i] = uint32_t(w.integer);
    }
    const Json* pw = body.find("password_required");
    if (!pw || pw->kind != Json::Kind::Bool) return "Field password_required must be a boolean.";
    passwordRequired = pw->boolean;
    return "";
}

bool parseBody(const HttpRequest& req, Json& out, HttpResponse& error) {
    if (req.body.size() > kMaxBodyBytes) {
        error = badRequest("The body is over 4 KB.");
        return false;
    }
    auto j = parseJson(req.body);
    if (!j || j->kind != Json::Kind::Object) {
        error = badRequest("The body is not a JSON object.");
        return false;
    }
    out = std::move(*j);
    return true;
}

} // namespace

HttpResponse Lobby::doRegister(const HttpRequest& req, uint64_t nowMs) {
    Json body;
    HttpResponse err;
    if (!parseBody(req, body, err)) return err;
    std::string why;
    int64_t protocol = 0;
    if (!intField(body, "protocol", 0, 1000000, protocol, why)) return badRequest(why);
    std::string application;
    if (!stringField(body, "application", application, why)) return badRequest(why);
    std::string app = normalizeGuid(application);
    if (app.empty()) return badRequest("The application is not a GUID.");
    Session s;
    why = readDescription(body, s.name, s.flags, s.maxPlayers, s.currentPlayers, s.user, s.passwordRequired);
    if (!why.empty()) return badRequest(why);
    if (protocol != kProtocolVersion) return unsupportedVersion();
    if (sessions_.size() >= config_.maxSessions) {
        return errorResponse(503, "unavailable", "The session table is full.", 30);
    }
    s.id = newSessionId();
    s.protocol = int(protocol);
    s.application = app;
    s.hostSecret = newSecret();
    s.createdMs = nowMs;
    s.lastBeatMs = nowMs;
    std::string ticket = issueTicket(s.id, 1, kRoleHost, nowMs);

    Json j = Json::makeObject();
    j.set("session_id", Json::makeString(hexId(s.id, 16)));
    j.set("peer_id", Json::makeString("00000001"));
    j.set("host_secret", Json::makeString(s.hostSecret));
    j.set("ticket", Json::makeString(ticket));
    j.set("rendezvous", rendezvousAddress());
    j.set("heartbeat_s", Json::makeInt(kHeartbeatSeconds));
    j.set("ttl_s", Json::makeInt(kSessionTtlSeconds));
    log::info(kTag, "session " + hexId(s.id, 16) + " registered, " + std::to_string(s.currentPlayers) +
                        " players");
    sessions_[s.id] = std::move(s);
    return jsonResponse(201, j);
}

HttpResponse Lobby::doHeartbeat(const HttpRequest& req, uint64_t sessionId, uint64_t nowMs) {
    Json body;
    HttpResponse err;
    if (!parseBody(req, body, err)) return err;
    std::string why;
    int64_t protocol = 0;
    if (!intField(body, "protocol", 0, 1000000, protocol, why)) return badRequest(why);
    std::string secret;
    if (!stringField(body, "host_secret", secret, why)) return badRequest(why);
    Description d;
    why = readDescription(body, d.name, d.flags, d.maxPlayers, d.currentPlayers, d.user, d.passwordRequired);
    if (!why.empty()) return badRequest(why);

    auto it = sessions_.find(sessionId);
    if (it == sessions_.end() || nowMs >= it->second.lastBeatMs + uint64_t(kSessionTtlSeconds) * 1000) {
        if (it != sessions_.end()) removeSession(sessionId, "no heartbeat");
        return notFound();
    }
    Session& s = it->second;
    if (protocol != s.protocol || protocol != kProtocolVersion) return unsupportedVersion();
    if (!constantTimeEqual(secret, s.hostSecret)) return forbidden("The host secret is wrong.");
    s.name = d.name;
    s.flags = d.flags;
    s.maxPlayers = d.maxPlayers;
    s.currentPlayers = d.currentPlayers;
    std::copy(d.user, d.user + 4, s.user);
    s.passwordRequired = d.passwordRequired;
    s.lastBeatMs = nowMs;
    Json j = Json::makeObject();
    j.set("heartbeat_s", Json::makeInt(kHeartbeatSeconds));
    j.set("ttl_s", Json::makeInt(kSessionTtlSeconds));
    return jsonResponse(200, j);
}

HttpResponse Lobby::doDelete(const HttpRequest& req, uint64_t sessionId, uint64_t nowMs) {
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end() || nowMs >= it->second.lastBeatMs + uint64_t(kSessionTtlSeconds) * 1000) {
        if (it != sessions_.end()) removeSession(sessionId, "no heartbeat");
        return notFound();
    }
    const std::string* secret = req.header("x-host-secret");
    if (!secret) return forbidden("The X-Host-Secret header is missing.");
    if (!constantTimeEqual(*secret, it->second.hostSecret)) return forbidden("The host secret is wrong.");
    removeSession(sessionId, "closed by host");
    HttpResponse r;
    r.status = 204;
    return r;
}

HttpResponse Lobby::doTicket(const HttpRequest& req, uint64_t sessionId, uint64_t nowMs) {
    Json body;
    HttpResponse err;
    if (!parseBody(req, body, err)) return err;
    std::string why;
    int64_t protocol = 0;
    if (!intField(body, "protocol", 0, 1000000, protocol, why)) return badRequest(why);

    bool renew = false;
    uint64_t renewPeer = 0;
    std::vector<uint8_t> renewTicket;
    if (const Json* r = body.find("renew")) {
        if (r->kind != Json::Kind::Object) return badRequest("Field renew is not an object.");
        std::string peerText, ticketText;
        if (!stringField(*r, "peer_id", peerText, why) || !stringField(*r, "ticket", ticketText, why)) {
            return badRequest(why);
        }
        if (!parseHexId(peerText, 8, renewPeer) || renewPeer == 0) return badRequest("The renewal peer_id is bad.");
        auto decoded = base64UrlDecode(ticketText);
        if (!decoded || decoded->size() != kTicketSize) return badRequest("The renewal ticket is bad.");
        renewTicket = std::move(*decoded);
        renew = true;
    }

    auto it = sessions_.find(sessionId);
    if (it == sessions_.end() || nowMs >= it->second.lastBeatMs + uint64_t(kSessionTtlSeconds) * 1000) {
        if (it != sessions_.end()) removeSession(sessionId, "no heartbeat");
        return notFound();
    }
    Session& s = it->second;
    if (protocol != s.protocol) return unsupportedVersion();

    uint8_t role = kRoleJoiner;
    if (renew) {
        auto t = checkTicket(config_.ticketKey, renewTicket.data(), renewTicket.size());
        if (!t || t->sessionId != sessionId || t->peerId != renewPeer) {
            return forbidden("The renewal ticket is not this session's.");
        }
        role = t->role;
    }

    while (!s.ticketTimes.empty() && nowMs >= s.ticketTimes.front() + uint64_t(kTicketLifetimeSeconds) * 1000) {
        s.ticketTimes.pop_front();
    }
    if (s.ticketTimes.size() >= size_t(kTicketBudget)) {
        return errorResponse(409, "session_full", "The session's ticket budget is spent.");
    }
    s.ticketTimes.push_back(nowMs);
    uint32_t peer = renew ? uint32_t(renewPeer) : s.nextPeer++;

    Json j = Json::makeObject();
    j.set("peer_id", Json::makeString(hexId(peer, 8)));
    j.set("ticket", Json::makeString(issueTicket(sessionId, peer, role, nowMs)));
    j.set("expires_s", Json::makeInt(kTicketLifetimeSeconds));
    j.set("rendezvous", rendezvousAddress());
    log::debug(kTag, "ticket issued for session " + hexId(sessionId, 16) + " peer " + hexId(peer, 8));
    return jsonResponse(201, j);
}

} // namespace sfcop::lobby
