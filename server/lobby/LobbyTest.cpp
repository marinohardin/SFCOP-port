// The HTTP handlers through an in-process client: hand-written requests, expected bodies and
// statuses written out from LOBBY_API.md, one test per error code. No sockets, no real clock.
#include "Base64Url.h"
#include "Lobby.h"
#include "Log.h"

#include <doctest/doctest.h>

#include <stdexcept>

using namespace sfcop::lobby;

namespace {

constexpr const char* kApp = "{0AB1C531-4745-11D1-A7A1-0000F803ABFC}";
constexpr const char* kUser = "198.51.100.7"; // RFC 5737 documentation range
constexpr uint64_t kStart = 1700000000000ull;  // a fixed instant, ms since the Unix epoch

TicketKey testKey() {
    TicketKey k{};
    for (size_t i = 0; i < k.size(); ++i) k[i] = uint8_t(0xA0 + i);
    return k;
}

// Each call fills with its own call number: the first (a session id) is eight 0x01 bytes, the
// second (the host secret) sixteen 0x02 bytes, and so on.
struct CountingRandom {
    int calls = 0;
    void operator()(uint8_t* out, size_t size) {
        ++calls;
        for (size_t i = 0; i < size; ++i) out[i] = uint8_t(calls);
    }
};

struct RecordingRelay : RelayClient {
    std::vector<uint64_t> removed;
    void removeSession(uint64_t id) override { removed.push_back(id); }
};

struct Fixture {
    CountingRandom random;
    RecordingRelay relay;
    LobbyConfig config;
    std::unique_ptr<Lobby> lobby;
    uint64_t now = kStart;

    explicit Fixture(size_t maxSessions = kMaxSessions) {
        config.rendezvousHost = "rv.example.test";
        config.rendezvousPort = 47710;
        config.ticketKey = testKey();
        config.maxSessions = maxSessions;
        lobby = std::make_unique<Lobby>(config, [this](uint8_t* o, size_t n) { random(o, n); }, kStart);
        lobby->setRelayClient(&relay);
    }

    HttpResponse call(const std::string& method, const std::string& target, const std::string& body = "",
                      const std::string& source = kUser,
                      const std::map<std::string, std::string>& headers = {}) {
        HttpRequest r;
        r.method = method;
        size_t q = target.find('?');
        r.path = target.substr(0, q);
        if (q != std::string::npos) r.query = target.substr(q + 1);
        r.body = body;
        r.source = source;
        r.headers = headers;
        return lobby->handle(r, now);
    }
};

std::string registerBody(const std::string& name = "Game One", int current = 1) {
    return std::string("{\"protocol\":1,\"application\":\"") + kApp + "\",\"name\":\"" + name +
           "\",\"flags\":8,\"max_players\":6,\"current_players\":" + std::to_string(current) +
           ",\"user\":[1,2,3,4000000000],\"password_required\":false}";
}

std::string heartbeatBody(const std::string& secret, const std::string& name = "Game One", int current = 1) {
    return std::string("{\"protocol\":1,\"host_secret\":\"") + secret + "\",\"name\":\"" + name +
           "\",\"flags\":8,\"max_players\":6,\"current_players\":" + std::to_string(current) +
           ",\"user\":[1,2,3,4],\"password_required\":true}";
}

Json parse(const HttpResponse& r) {
    auto j = parseJson(r.body);
    REQUIRE(j.has_value());
    return *j;
}

std::string errorCode(const HttpResponse& r) {
    Json j = parse(r);
    const Json* e = j.find("error");
    REQUIRE(e != nullptr);
    return e->find("code")->string;
}

const std::string kSecret = "AgICAgICAgICAgICAgICAg"; // sixteen 0x02 bytes
const std::string kSession = "0101010101010101";

HttpResponse registerOne(Fixture& f) {
    HttpResponse r = f.call("POST", "/v1/sessions", registerBody());
    REQUIRE(r.status == 201);
    return r;
}

} // namespace

TEST_CASE("health answers ok with the api, the protocols, the session count and the uptime") {
    Fixture f;
    f.now = kStart + 90500;
    HttpResponse r = f.call("GET", "/v1/health", "", "127.0.0.1");
    CHECK(r.status == 200);
    CHECK(r.body == R"({"api":1,"protocols":[1],"sessions":0,"status":"ok","uptime_s":90})");
}

TEST_CASE("registration returns the session, the host's ticket, the secret and the rendezvous") {
    Fixture f;
    HttpResponse r = registerOne(f);
    Json j = parse(r);
    CHECK(j.find("session_id")->string == kSession);
    CHECK(j.find("peer_id")->string == "00000001");
    CHECK(j.find("host_secret")->string == kSecret);
    CHECK(j.find("heartbeat_s")->integer == 15);
    CHECK(j.find("ttl_s")->integer == 45);
    CHECK(j.find("rendezvous")->find("host")->string == "rv.example.test");
    CHECK(j.find("rendezvous")->find("port")->integer == 47710);

    // The ticket is 36 bytes in 48 characters and says: this session, peer 1, host, ten minutes.
    const std::string& text = j.find("ticket")->string;
    CHECK(text.size() == 48);
    auto bytes = base64UrlDecode(text);
    REQUIRE(bytes.has_value());
    auto t = checkTicket(testKey(), bytes->data(), bytes->size());
    REQUIRE(t.has_value());
    CHECK(t->sessionId == 0x0101010101010101ull);
    CHECK(t->peerId == 1);
    CHECK(t->role == kRoleHost);
    CHECK(t->expiresAt == 1700000000u + 600u);
    CHECK(f.lobby->sessionCount() == 1);
}

TEST_CASE("the list shows a session with its flags, newest first, and never an address") {
    Fixture f;
    registerOne(f);
    f.now += 20000;
    HttpResponse second = f.call("POST", "/v1/sessions", registerBody("Game Two", 3));
    REQUIRE(second.status == 201);
    f.now += 5000;

    HttpResponse r = f.call("GET", std::string("/v1/sessions?protocol=1&application=%7B0ab1c531-4745-11d1-a7a1-0000f803abfc%7D"));
    REQUIRE(r.status == 200);
    Json j = parse(r);
    CHECK(j.find("next_poll_s")->integer == 5);
    const auto& list = j.find("sessions")->array;
    REQUIRE(list.size() == 2);
    CHECK(list[0].find("name")->string == "Game Two");
    CHECK(list[0].find("age_s")->integer == 5);
    CHECK(list[0].find("current_players")->integer == 3);
    CHECK(list[1].find("name")->string == "Game One");
    CHECK(list[1].find("age_s")->integer == 25);
    CHECK(list[1].find("flags")->integer == 8);
    CHECK(list[1].find("max_players")->integer == 6);
    CHECK(list[1].find("protocol")->integer == 1);
    CHECK(list[1].find("application")->string == kApp);
    CHECK(list[1].find("user")->dump() == "[1,2,3,4000000000]");
    CHECK(r.body.find("198.51.100") == std::string::npos);
    CHECK(r.body.find("secret") == std::string::npos);
}

TEST_CASE("the list holds only the application asked for, and at most 200 sessions") {
    Fixture f;
    registerOne(f);
    HttpResponse other = f.call("GET", "/v1/sessions?protocol=1&application=%7B00000000-0000-0000-0000-000000000000%7D");
    CHECK(other.body == R"({"next_poll_s":5,"sessions":[]})");

    Fixture many;
    for (int i = 0; i < 205; ++i) {
        // a new source each time keeps clear of the register limit
        REQUIRE(many.call("POST", "/v1/sessions", registerBody("G" + std::to_string(i)), "10.0." + std::to_string(i / 200) + "." + std::to_string(i % 200)).status == 201);
    }
    HttpResponse r = many.call("GET", std::string("/v1/sessions?protocol=1&application=") + "%7B0AB1C531-4745-11D1-A7A1-0000F803ABFC%7D");
    CHECK(parse(r).find("sessions")->array.size() == 200);
}

TEST_CASE("a heartbeat updates the description and the password flag; a session lapses after 45 s") {
    Fixture f;
    registerOne(f);
    f.now += 15000;
    HttpResponse hb = f.call("PUT", "/v1/sessions/" + kSession, heartbeatBody(kSecret, "Renamed", 4));
    CHECK(hb.status == 200);
    CHECK(hb.body == R"({"heartbeat_s":15,"ttl_s":45})");

    HttpResponse r = f.call("GET", std::string("/v1/sessions?protocol=1&application=%7B0AB1C531-4745-11D1-A7A1-0000F803ABFC%7D"));
    Json entry = parse(r).find("sessions")->array.at(0);
    CHECK(entry.find("name")->string == "Renamed");
    CHECK(entry.find("current_players")->integer == 4);
    CHECK(entry.find("flags")->integer == (8 | 0x400)); // DPSESSION_PASSWORDREQUIRED added by the lobby

    f.now += 44999;
    CHECK(f.call("PUT", "/v1/sessions/" + kSession, heartbeatBody(kSecret)).status == 200);
    f.now += 45000;
    CHECK(f.call("PUT", "/v1/sessions/" + kSession, heartbeatBody(kSecret)).status == 404); // lapsed
}

TEST_CASE("sweeping removes a lapsed session and tells the relay") {
    Fixture f;
    registerOne(f);
    f.now += 44999;
    f.lobby->sweep(f.now);
    CHECK(f.lobby->sessionCount() == 1);
    f.now += 1;
    f.lobby->sweep(f.now);
    CHECK(f.lobby->sessionCount() == 0);
    REQUIRE(f.relay.removed.size() == 1);
    CHECK(f.relay.removed[0] == 0x0101010101010101ull);
    CHECK(parse(f.call("GET", "/v1/health", "", "127.0.0.1")).find("sessions")->integer == 0);
}

TEST_CASE("a lapsed session is already gone from the list before the sweep runs") {
    Fixture f;
    registerOne(f);
    f.now += 45000;
    HttpResponse r = f.call("GET", std::string("/v1/sessions?protocol=1&application=%7B0AB1C531-4745-11D1-A7A1-0000F803ABFC%7D"));
    CHECK(parse(r).find("sessions")->array.empty());
}

TEST_CASE("removal with the host secret answers 204 and tells the relay") {
    Fixture f;
    registerOne(f);
    HttpResponse r = f.call("DELETE", "/v1/sessions/" + kSession, "", kUser, {{"x-host-secret", kSecret}});
    CHECK(r.status == 204);
    CHECK(r.body.empty());
    CHECK(f.lobby->sessionCount() == 0);
    REQUIRE(f.relay.removed.size() == 1);
    CHECK(f.relay.removed[0] == 0x0101010101010101ull);
}

TEST_CASE("error bad_request: malformed JSON, a missing or out-of-range field, a bad id, a big body") {
    Fixture f;
    CHECK(errorCode(f.call("POST", "/v1/sessions", "{not json")) == "bad_request");
    CHECK(f.call("POST", "/v1/sessions", "[1]").status == 400);
    CHECK(errorCode(f.call("POST", "/v1/sessions", R"({"protocol":1})")) == "bad_request");

    // each field out of range in turn, each from its own source so the register limit stays out of it
    int source = 0;
    for (const char* patch : {R"("name":"")", R"("max_players":65)", R"("current_players":7)", R"("flags":4294967296)",
                              R"("user":[1,2,3])", R"("user":[1,2,3,-1])", R"("password_required":"no")",
                              R"("application":"not-a-guid")", R"("protocol":1.5)"}) {
        std::string body = registerBody();
        // replace the field by appending the patch last; the parser keeps the last duplicate
        body.pop_back();
        body += std::string(",") + patch + "}";
        CAPTURE(patch);
        CHECK(errorCode(f.call("POST", "/v1/sessions", body, "203.0.113." + std::to_string(++source))) == "bad_request");
    }
    CHECK(errorCode(f.call("POST", "/v1/sessions", registerBody(std::string(64, 'x')), "192.0.2.1")) == "bad_request");

    // a name of 63 bytes is fine
    CHECK(f.call("POST", "/v1/sessions", registerBody(std::string(63, 'x')), "192.0.2.2").status == 201);

    CHECK(errorCode(f.call("POST", "/v1/sessions", std::string(4097, ' '), "192.0.2.3")) == "bad_request");
    CHECK(errorCode(f.call("PUT", "/v1/sessions/NOTANID", heartbeatBody("x"), "192.0.2.4")) == "bad_request");
    CHECK(errorCode(f.call("GET", "/v1/sessions?protocol=x&application=%7B0AB1C531-4745-11D1-A7A1-0000F803ABFC%7D", "", "192.0.2.5")) == "bad_request");
    CHECK(errorCode(f.call("GET", "/v1/sessions?protocol=1", "", "192.0.2.6")) == "bad_request");
    CHECK(errorCode(f.call("GET", "/v1/sessions?protocol=1&application=%zz", "", "192.0.2.7")) == "bad_request");
}

TEST_CASE("error unsupported_version: a protocol the lobby does not serve") {
    Fixture f;
    std::string body = registerBody();
    body.replace(body.find("\"protocol\":1"), 12, "\"protocol\":2");
    HttpResponse r = f.call("POST", "/v1/sessions", body);
    CHECK(r.status == 400);
    CHECK(errorCode(r) == "unsupported_version");

    CHECK(errorCode(f.call("GET", "/v1/sessions?protocol=2&application=%7B0AB1C531-4745-11D1-A7A1-0000F803ABFC%7D")) == "unsupported_version");

    registerOne(f);
    std::string hb = heartbeatBody(kSecret);
    hb.replace(hb.find("\"protocol\":1"), 12, "\"protocol\":2");
    CHECK(errorCode(f.call("PUT", "/v1/sessions/" + kSession, hb)) == "unsupported_version");
    CHECK(errorCode(f.call("POST", "/v1/sessions/" + kSession + "/tickets", R"({"protocol":2})")) == "unsupported_version");
}

TEST_CASE("error forbidden: a wrong host secret, a missing header, a foreign renewal, a blocked address") {
    Fixture f;
    registerOne(f);
    HttpResponse r = f.call("PUT", "/v1/sessions/" + kSession, heartbeatBody("AAAAAAAAAAAAAAAAAAAAAA"));
    CHECK(r.status == 403);
    CHECK(errorCode(r) == "forbidden");
    CHECK(errorCode(f.call("DELETE", "/v1/sessions/" + kSession, "", kUser, {{"x-host-secret", "AAAAAAAAAAAAAAAAAAAAAA"}})) == "forbidden");
    CHECK(errorCode(f.call("DELETE", "/v1/sessions/" + kSession)) == "forbidden");
    CHECK(f.lobby->sessionCount() == 1);

    // a renewal whose ticket is another session's: made with the right key but a different session
    TicketBody other;
    other.sessionId = 0x0909090909090909ull;
    other.peerId = 2;
    other.expiresAt = 1;
    other.role = kRoleJoiner;
    TicketBytes t = makeTicket(testKey(), other);
    std::string renew = std::string(R"({"protocol":1,"renew":{"peer_id":"00000002","ticket":")") +
                        base64UrlEncode(t.data(), t.size()) + "\"}}";
    CHECK(errorCode(f.call("POST", "/v1/sessions/" + kSession + "/tickets", renew)) == "forbidden");

    // a ticket whose mac is wrong
    other.sessionId = 0x0101010101010101ull;
    TicketBytes right = makeTicket(testKey(), other);
    right[35] ^= 1;
    renew = std::string(R"({"protocol":1,"renew":{"peer_id":"00000002","ticket":")") +
            base64UrlEncode(right.data(), right.size()) + "\"}}";
    CHECK(errorCode(f.call("POST", "/v1/sessions/" + kSession + "/tickets", renew)) == "forbidden");

    f.lobby->setBlocked({"203.0.113.50"});
    HttpResponse blocked = f.call("GET", "/v1/sessions?protocol=1&application=%7B0AB1C531-4745-11D1-A7A1-0000F803ABFC%7D", "", "203.0.113.50");
    CHECK(blocked.status == 403);
    CHECK(errorCode(blocked) == "forbidden");
}

TEST_CASE("error not_found: no such session, no such path, a method the path does not take") {
    Fixture f;
    CHECK(errorCode(f.call("PUT", "/v1/sessions/ffffffffffffffff", heartbeatBody("x"))) == "not_found");
    CHECK(f.call("DELETE", "/v1/sessions/ffffffffffffffff").status == 404);
    CHECK(errorCode(f.call("POST", "/v1/sessions/ffffffffffffffff/tickets", R"({"protocol":1})")) == "not_found");
    CHECK(f.call("GET", "/v2/sessions").status == 404);
    CHECK(f.call("GET", "/").status == 404);
    CHECK(f.call("GET", "/v1/sessions/0101010101010101/other").status == 404);
    CHECK(f.call("DELETE", "/v1/sessions").status == 404);
    CHECK(f.call("POST", "/v1/health", "", "127.0.0.1").status == 404);
    CHECK(f.call("GET", "/v1/sessions/0101010101010101").status == 404);
}

TEST_CASE("tickets are given in order from peer 2 and a renewal keeps the peer id") {
    Fixture f;
    registerOne(f);
    HttpResponse a = f.call("POST", "/v1/sessions/" + kSession + "/tickets", R"({"protocol":1})");
    REQUIRE(a.status == 201);
    Json ja = parse(a);
    CHECK(ja.find("peer_id")->string == "00000002");
    CHECK(ja.find("expires_s")->integer == 600);
    CHECK(ja.find("rendezvous")->find("host")->string == "rv.example.test");
    auto bytes = base64UrlDecode(ja.find("ticket")->string);
    REQUIRE(bytes.has_value());
    auto t = checkTicket(testKey(), bytes->data(), bytes->size());
    REQUIRE(t.has_value());
    CHECK(t->peerId == 2);
    CHECK(t->role == kRoleJoiner);

    HttpResponse b = f.call("POST", "/v1/sessions/" + kSession + "/tickets", R"({"protocol":1})");
    CHECK(parse(b).find("peer_id")->string == "00000003");

    // twelve minutes later the first ticket has expired; the renewal still works while the session lives
    for (int i = 0; i < 48; ++i) {
        f.now += 15000;
        REQUIRE(f.call("PUT", "/v1/sessions/" + kSession, heartbeatBody(kSecret)).status == 200);
    }
    std::string renew = std::string(R"({"protocol":1,"renew":{"peer_id":"00000002","ticket":")") +
                        ja.find("ticket")->string + "\"}}";
    HttpResponse again = f.call("POST", "/v1/sessions/" + kSession + "/tickets", renew);
    REQUIRE(again.status == 201);
    Json jr = parse(again);
    CHECK(jr.find("peer_id")->string == "00000002");
    auto rb = base64UrlDecode(jr.find("ticket")->string);
    auto rt = checkTicket(testKey(), rb->data(), rb->size());
    REQUIRE(rt.has_value());
    CHECK(rt->peerId == 2);
    CHECK(rt->expiresAt == uint32_t((f.now / 1000) + 600));

    // the next new joiner is still peer 4
    CHECK(parse(f.call("POST", "/v1/sessions/" + kSession + "/tickets", R"({"protocol":1})")).find("peer_id")->string == "00000004");
}

TEST_CASE("a renewal with a bad encoding is bad_request") {
    Fixture f;
    registerOne(f);
    CHECK(errorCode(f.call("POST", "/v1/sessions/" + kSession + "/tickets",
                           R"({"protocol":1,"renew":{"peer_id":"00000002","ticket":"!!"}})")) == "bad_request");
    CHECK(errorCode(f.call("POST", "/v1/sessions/" + kSession + "/tickets",
                           R"({"protocol":1,"renew":{"peer_id":"2","ticket":"AAAA"}})")) == "bad_request");
    CHECK(errorCode(f.call("POST", "/v1/sessions/" + kSession + "/tickets", R"({"protocol":1,"renew":5})")) == "bad_request");
}

TEST_CASE("error session_full: 64 tickets in ten minutes, renewals included, then room again") {
    Fixture f;
    registerOne(f);
    for (int i = 0; i < 64; ++i) {
        f.now += 100;
        // 30 a minute per source: use a source per request
        HttpResponse r = f.call("POST", "/v1/sessions/" + kSession + "/tickets", R"({"protocol":1})", "192.0.2." + std::to_string(i + 1));
        REQUIRE(r.status == 201);
    }
    f.now += 100;
    HttpResponse full = f.call("POST", "/v1/sessions/" + kSession + "/tickets", R"({"protocol":1})", "192.0.2.200");
    CHECK(full.status == 409);
    CHECK(errorCode(full) == "session_full");
    // keep the session alive across ten minutes, then the oldest tickets have aged out
    for (int i = 0; i < 40; ++i) {
        f.now += 15000;
        REQUIRE(f.call("PUT", "/v1/sessions/" + kSession, heartbeatBody(kSecret), "192.0.2." + std::to_string(i + 1)).status == 200);
    }
    CHECK(f.call("POST", "/v1/sessions/" + kSession + "/tickets", R"({"protocol":1})", "192.0.2.201").status == 201);
}

TEST_CASE("error rate_limited: over a limit the request answers 429 with Retry-After") {
    Fixture f;
    for (int i = 0; i < 6; ++i) CHECK(f.call("POST", "/v1/sessions", registerBody()).status == 201);
    HttpResponse r = f.call("POST", "/v1/sessions", registerBody());
    CHECK(r.status == 429);
    CHECK(errorCode(r) == "rate_limited");
    CHECK(parse(r).find("error")->find("retry_after_s")->integer == 10);
    CHECK(r.headers.at("Retry-After") == "10");
    CHECK(f.lobby->sessionCount() == 6); // the refused request was not processed
    f.now += 10000;
    CHECK(f.call("POST", "/v1/sessions", registerBody()).status == 201);
}

TEST_CASE("the limits of the other endpoints and the all-requests bucket") {
    Fixture f;
    const std::string list = "/v1/sessions?protocol=1&application=%7B0AB1C531-4745-11D1-A7A1-0000F803ABFC%7D";
    for (int i = 0; i < 30; ++i) CHECK(f.call("GET", list).status == 200);
    CHECK(f.call("GET", list).status == 429);

    Fixture g;
    registerOne(g);
    for (int i = 0; i < 10; ++i) CHECK(g.call("PUT", "/v1/sessions/" + kSession, heartbeatBody(kSecret)).status == 200);
    CHECK(g.call("PUT", "/v1/sessions/" + kSession, heartbeatBody(kSecret)).status == 429);

    Fixture h;
    registerOne(h);
    for (int i = 0; i < 6; ++i) CHECK(h.call("DELETE", "/v1/sessions/ffffffffffffffff").status == 404);
    CHECK(h.call("DELETE", "/v1/sessions/ffffffffffffffff").status == 429);

    Fixture t;
    registerOne(t);
    for (int i = 0; i < 30; ++i) CHECK(t.call("POST", "/v1/sessions/" + kSession + "/tickets", R"({"protocol":1})").status == 201);
    CHECK(t.call("POST", "/v1/sessions/" + kSession + "/tickets", R"({"protocol":1})").status == 429);

    // 120 requests of any kind in a minute: unknown paths spend only the all-requests bucket
    Fixture a;
    for (int i = 0; i < 120; ++i) CHECK(a.call("GET", "/nothing").status == 404);
    CHECK(a.call("GET", "/nothing").status == 429);
    CHECK(a.call("GET", "/nothing", "", "192.0.2.9").status == 404); // another source is not affected
}

TEST_CASE("health is unlimited from loopback and six a minute from anywhere else") {
    Fixture f;
    for (int i = 0; i < 200; ++i) CHECK(f.call("GET", "/v1/health", "", "127.0.0.1").status == 200);
    for (int i = 0; i < 6; ++i) CHECK(f.call("GET", "/v1/health", "", "192.0.2.77").status == 200);
    CHECK(errorCode(f.call("GET", "/v1/health", "", "192.0.2.77")) == "rate_limited");
}

TEST_CASE("error unavailable: the session table is full") {
    Fixture f(1);
    registerOne(f);
    HttpResponse r = f.call("POST", "/v1/sessions", registerBody("Another"), "192.0.2.1");
    CHECK(r.status == 503);
    CHECK(errorCode(r) == "unavailable");
    CHECK(parse(r).find("error")->find("retry_after_s")->integer == 30);
    CHECK(r.headers.at("Retry-After") == "30");
}

TEST_CASE("error internal: a defect inside the lobby answers 500 and the lobby goes on") {
    Fixture f;
    f.lobby = std::make_unique<Lobby>(f.config, [](uint8_t*, size_t) { throw std::runtime_error("no entropy"); }, kStart);
    HttpResponse r = f.call("POST", "/v1/sessions", registerBody());
    CHECK(r.status == 500);
    CHECK(errorCode(r) == "internal");
    CHECK(f.call("GET", "/v1/health", "", "127.0.0.1").status == 200);
}

TEST_CASE("every error body has a code and an English message") {
    Fixture f;
    HttpResponse r = f.call("GET", "/nope");
    Json e = *parse(r).find("error");
    CHECK(e.find("code")->string == "not_found");
    CHECK(e.find("message")->kind == Json::Kind::String);
    CHECK_FALSE(e.find("message")->string.empty());
}

namespace {
struct LogCapture {
    std::vector<std::pair<LogLevel, std::string>> lines;
    LogCapture() {
        sfcop::lobby::log::setSink([this](LogLevel l, const std::string& s) { lines.emplace_back(l, s); });
    }
    ~LogCapture() { sfcop::lobby::log::setSink([](LogLevel, const std::string&) {}); } // back to silent
};
} // namespace

TEST_CASE("the log lines of a session's life carry no address, name, secret or ticket") {
    LogCapture capture;
    Fixture f;
    const std::string name = "TestCaptain's Secret Game";
    HttpResponse reg = f.call("POST", "/v1/sessions", registerBody(name));
    REQUIRE(reg.status == 201);
    std::string ticket = parse(reg).find("ticket")->string;
    f.call("PUT", "/v1/sessions/" + kSession, heartbeatBody(kSecret, name));
    HttpResponse tk = f.call("POST", "/v1/sessions/" + kSession + "/tickets", R"({"protocol":1})", "203.0.113.5");
    std::string joinerTicket = parse(tk).find("ticket")->string;
    f.call("GET", "/v1/sessions?protocol=1&application=%7B0AB1C531-4745-11D1-A7A1-0000F803ABFC%7D", "", "203.0.113.6");
    f.call("PUT", "/v1/sessions/" + kSession, heartbeatBody("wrongwrongwrongwrongww"));
    f.call("DELETE", "/v1/sessions/" + kSession, "", kUser, {{"x-host-secret", kSecret}});

    REQUIRE_FALSE(capture.lines.empty());
    bool sawRegistered = false, sawRemoved = false;
    for (const auto& line : capture.lines) {
        CAPTURE(line.second);
        CHECK(line.first <= LogLevel::Info);
        CHECK(line.second.find("198.51.100") == std::string::npos);
        CHECK(line.second.find("203.0.113") == std::string::npos);
        CHECK(line.second.find("TestCaptain") == std::string::npos);
        CHECK(line.second.find(kSecret) == std::string::npos);
        CHECK(line.second.find(ticket) == std::string::npos);
        CHECK(line.second.find(joinerTicket) == std::string::npos);
        sawRegistered |= line.second == "lobby: session 0101010101010101 registered, 1 players";
        sawRemoved |= line.second == "lobby: session 0101010101010101 removed (closed by host), 1 players";
    }
    CHECK(sawRegistered);
    CHECK(sawRemoved);
}

TEST_CASE("a rate-limit event is logged once a minute with the source address, as the document says") {
    LogCapture capture;
    Fixture f;
    for (int i = 0; i < 20; ++i) f.call("POST", "/v1/sessions", registerBody(), "203.0.113.5");
    int events = 0;
    for (const auto& line : capture.lines) {
        if (line.second.find("rate limit reached by 203.0.113.5") != std::string::npos) {
            ++events;
            CHECK(line.first == LogLevel::Warn);
        }
    }
    CHECK(events == 1);
}

TEST_CASE("a blocked address is logged once with its address and then ignored") {
    LogCapture capture;
    Fixture f;
    f.lobby->setBlocked({"203.0.113.5"});
    for (int i = 0; i < 5; ++i) CHECK(f.call("GET", "/v1/health", "", "203.0.113.5").status == 403);
    int events = 0;
    for (const auto& line : capture.lines) events += line.second == "lobby: blocked address 203.0.113.5 refused";
    CHECK(events == 1);
}

TEST_CASE("the per-request log line names the route pattern but not the session or the address") {
    Fixture f;
    registerOne(f);
    HttpResponse r = f.call("PUT", "/v1/sessions/" + kSession, heartbeatBody(kSecret));
    CHECK(requestLogLine("PUT", r, 3) == "PUT /v1/sessions/<session> 200 3ms");
    HttpResponse missing = f.call("GET", "/nowhere");
    CHECK(requestLogLine("GET", missing, 0) == "GET (unrouted) 404 0ms");
}

TEST_CASE("a restart loses the list and nothing else: a heartbeat for the old session is not_found") {
    Fixture before;
    registerOne(before);
    Fixture after; // a new process: same key, empty table
    CHECK(errorCode(after.call("PUT", "/v1/sessions/" + kSession, heartbeatBody(kSecret))) == "not_found");
    CHECK(after.lobby->sessionCount() == 0);
}

TEST_CASE("a repeat DELETE answers not_found and tells the relay once") {
    Fixture f;
    registerOne(f);
    CHECK(f.call("DELETE", "/v1/sessions/" + kSession, "", kUser, {{"x-host-secret", kSecret}}).status == 204);
    // a host retrying a lost 204 gets not_found: the session is already gone
    HttpResponse again = f.call("DELETE", "/v1/sessions/" + kSession, "", kUser, {{"x-host-secret", kSecret}});
    CHECK(again.status == 404);
    CHECK(errorCode(again) == "not_found");
    CHECK(f.relay.removed.size() == 1);
}

TEST_CASE("a DELETE after the sweep removed the session is not_found and the relay was told once") {
    Fixture f;
    registerOne(f);
    f.now += 45000;
    f.lobby->sweep(f.now);
    CHECK(f.call("DELETE", "/v1/sessions/" + kSession, "", kUser, {{"x-host-secret", kSecret}}).status == 404);
    CHECK(f.relay.removed.size() == 1);
}

TEST_CASE("a repeated heartbeat leaves one session") {
    Fixture f;
    registerOne(f);
    for (int i = 0; i < 3; ++i) {
        f.now += 1000;
        CHECK(f.call("PUT", "/v1/sessions/" + kSession, heartbeatBody(kSecret)).status == 200);
    }
    CHECK(f.lobby->sessionCount() == 1);
}

TEST_CASE("an empty session id is not_found, not bad_request") {
    Fixture f;
    CHECK(f.call("PUT", "/v1/sessions/", heartbeatBody("x")).status == 404);
    CHECK(f.call("DELETE", "/v1/sessions/").status == 404);
    CHECK(f.call("POST", "/v1/sessions//tickets", R"({"protocol":1})").status == 404);
}

TEST_CASE("a sweep makes at most five relay calls and a request handler one more, the rest wait") {
    Fixture f;
    for (int i = 0; i < 7; ++i) {
        REQUIRE(f.call("POST", "/v1/sessions", registerBody("G" + std::to_string(i)), "10.1.0." + std::to_string(i)).status == 201);
    }
    f.now += 45000;
    f.lobby->sweep(f.now);
    CHECK(f.lobby->sessionCount() == 0);   // all seven are gone from the table at once
    CHECK(f.relay.removed.size() == 5);    // but the relay heard of five
    f.call("GET", "/v1/health", "", "127.0.0.1");
    CHECK(f.relay.removed.size() == 6);    // a handler makes one more call
    f.lobby->sweep(f.now + 1000);
    CHECK(f.relay.removed.size() == 7);
}

TEST_CASE("creating twice from the same client makes two sessions: the lobby has no client identity to merge on") {
    // The defined answer: every POST is a new session with its own id and secret, so a host that
    // lost the first answer registers again and the first lapses by its time to live.
    Fixture f;
    Json first = parse(registerOne(f));
    HttpResponse second = f.call("POST", "/v1/sessions", registerBody());
    REQUIRE(second.status == 201);
    Json b = parse(second);
    CHECK(first.find("session_id")->string == kSession);
    CHECK(b.find("session_id")->string != first.find("session_id")->string);
    CHECK(b.find("host_secret")->string != first.find("host_secret")->string);
    CHECK(f.lobby->sessionCount() == 2);
    f.now += 45000;
    f.lobby->sweep(f.now);
    CHECK(f.lobby->sessionCount() == 0); // both lapse unless their host heartbeats
}

TEST_CASE("asking for a ticket twice gives two peers; renewing twice gives the same peer and takes no new id") {
    Fixture f;
    registerOne(f);
    const std::string path = "/v1/sessions/" + kSession + "/tickets";
    Json a = parse(f.call("POST", path, R"({"protocol":1})"));
    Json b = parse(f.call("POST", path, R"({"protocol":1})"));
    CHECK(a.find("peer_id")->string == "00000002");
    CHECK(b.find("peer_id")->string == "00000003"); // without a renewal each request is a new machine

    std::string renew = std::string(R"({"protocol":1,"renew":{"peer_id":"00000002","ticket":")") +
                        a.find("ticket")->string + "\"}}";
    Json r1 = parse(f.call("POST", path, renew));
    Json r2 = parse(f.call("POST", path, renew));
    CHECK(r1.find("peer_id")->string == "00000002");
    CHECK(r2.find("peer_id")->string == "00000002");
    CHECK(parse(f.call("POST", path, R"({"protocol":1})")).find("peer_id")->string == "00000004");
}
