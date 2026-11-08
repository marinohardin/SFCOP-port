// JSON, HTTP, ticket, configuration and rate-limit units, each with hand-written input.
#include "Config.h"
#include "Http.h"
#include "Json.h"
#include "RateLimit.h"
#include "Ticket.h"

#include <doctest/doctest.h>

#include <map>

using namespace sfcop::lobby;

TEST_CASE("json parses objects, arrays, strings and integers") {
    auto j = parseJson(R"( {"a":1,"b":[1,2,3],"c":"x\u00e9\n","d":true,"e":null,"f":-7} )");
    REQUIRE(j.has_value());
    CHECK(j->find("a")->integer == 1);
    CHECK(j->find("b")->array.size() == 3);
    CHECK(j->find("c")->string == "x\xC3\xA9\n");
    CHECK(j->find("d")->boolean);
    CHECK(j->find("e")->kind == Json::Kind::Null);
    CHECK(j->find("f")->integer == -7);
    CHECK(j->find("missing") == nullptr);
}

TEST_CASE("json keeps non-integers apart from integers") {
    CHECK(parseJson("1.5")->kind == Json::Kind::Real);
    CHECK(parseJson("1e3")->kind == Json::Kind::Real);
    CHECK(parseJson("99999999999999999999")->kind == Json::Kind::Real);
    CHECK(parseJson("42")->kind == Json::Kind::Integer);
}

TEST_CASE("json refuses malformed text") {
    for (const char* bad : {"", "{", "{\"a\":}", "[1,]", "{\"a\":1,}", "01", "-", "\"abc", "{'a':1}", "[1] x",
                            "\"\\q\"", "\"\\ud800\"", "\"\x01\"", "tru", "{\"a\" 1}"}) {
        CAPTURE(bad);
        CHECK_FALSE(parseJson(bad).has_value());
    }
}

TEST_CASE("json refuses invalid UTF-8 and nesting past eight levels") {
    CHECK_FALSE(parseJson("\"\xC3\"").has_value());          // truncated sequence
    CHECK_FALSE(parseJson("\"\xC0\x80\"").has_value());      // overlong
    CHECK_FALSE(parseJson("\"\xED\xA0\x80\"").has_value());  // a surrogate
    CHECK(parseJson("[[[[[[[[1]]]]]]]]").has_value());       // eight levels
    CHECK_FALSE(parseJson("[[[[[[[[[1]]]]]]]]]").has_value());
}

TEST_CASE("json writes compact text with sorted keys and escapes") {
    Json o = Json::makeObject();
    o.set("b", Json::makeInt(2));
    o.set("a", Json::makeString("q\"\\\n\x01"));
    Json arr = Json::makeArray();
    arr.push(Json::makeBool(false));
    o.set("c", std::move(arr));
    CHECK(o.dump() == R"({"a":"q\"\\\n\u0001","b":2,"c":[false]})");
}

TEST_CASE("http parses a request with a body") {
    std::string raw = "POST /v1/sessions?x=1 HTTP/1.1\r\nHost: lobby.test\r\nContent-Length: 7\r\n"
                      "X-Host-Secret:  abc \r\n\r\n{\"a\":1}";
    ParseResult p = parseHttpRequest(raw);
    REQUIRE(p.state == ParseState::Done);
    CHECK(p.request.method == "POST");
    CHECK(p.request.path == "/v1/sessions");
    CHECK(p.request.query == "x=1");
    CHECK(p.request.body == "{\"a\":1}");
    REQUIRE(p.request.header("x-host-secret") != nullptr);
    CHECK(*p.request.header("x-host-secret") == "abc");
}

TEST_CASE("http waits for the rest of a split request") {
    CHECK(parseHttpRequest("GET /v1/health HTTP/1.1\r\nHost: a").state == ParseState::NeedMore);
    CHECK(parseHttpRequest("POST /x HTTP/1.1\r\nContent-Length: 5\r\n\r\nab").state == ParseState::NeedMore);
}

TEST_CASE("http refuses a request that is bad or too large") {
    CHECK(parseHttpRequest("GET\r\n\r\n").state == ParseState::Bad);
    CHECK(parseHttpRequest("GET /x HTTP/2\r\n\r\n").state == ParseState::Bad);
    CHECK(parseHttpRequest("GET x HTTP/1.1\r\n\r\n").state == ParseState::Bad);
    CHECK(parseHttpRequest("GET /x HTTP/1.1\r\nbroken\r\n\r\n").state == ParseState::Bad);
    CHECK(parseHttpRequest("POST /x HTTP/1.1\r\nContent-Length: abc\r\n\r\n").state == ParseState::Bad);
    CHECK(parseHttpRequest("POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n").state == ParseState::Bad);
    CHECK(parseHttpRequest("POST /x HTTP/1.1\r\nContent-Length: 4097\r\n\r\n").state == ParseState::TooLarge);
    CHECK(parseHttpRequest(std::string(9000, 'a')).state == ParseState::TooLarge);
}

TEST_CASE("http writes a response with its length and closes the connection") {
    HttpResponse r;
    r.status = 429;
    r.body = "{}";
    r.headers["Retry-After"] = "7";
    CHECK(serializeHttpResponse(r) ==
          "HTTP/1.1 429 Too Many Requests\r\nContent-Type: application/json\r\nContent-Length: 2\r\n"
          "Connection: close\r\nRetry-After: 7\r\n\r\n{}");
    HttpResponse none;
    none.status = 204;
    CHECK(serializeHttpResponse(none) == "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
}

TEST_CASE("percent decoding handles braces and refuses bad escapes") {
    std::string out;
    REQUIRE(percentDecode("%7B0AB1C531-4745%7d", out));
    CHECK(out == "{0AB1C531-4745}");
    CHECK_FALSE(percentDecode("%7", out));
    CHECK_FALSE(percentDecode("%zz", out));
}

namespace {
TicketKey testKey() {
    TicketKey k{};
    for (size_t i = 0; i < k.size(); ++i) k[i] = uint8_t(i);
    return k;
}
} // namespace

TEST_CASE("a ticket is 36 bytes laid out as RENDEZVOUS.md says") {
    TicketBody b;
    b.sessionId = 0x0102030405060708ull;
    b.peerId = 3;
    b.expiresAt = 0x5F5E1000;
    b.role = kRoleJoiner;
    TicketBytes t = makeTicket(testKey(), b);
    // little-endian session, peer, expiry; role; three reserved zeros
    const uint8_t head[20] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x03, 0x00,
                              0x00, 0x00, 0x00, 0x10, 0x5E, 0x5F, 0x02, 0x00, 0x00, 0x00};
    for (size_t i = 0; i < 20; ++i) CHECK(t[i] == head[i]);
    // The whole 36 bytes. The key is bytes 0x00..0x1f; the mac was computed once outside the code:
    //   python3 -c "import hmac,hashlib; print(hmac.new(bytes(range(32)), bytes.fromhex(
    //     '08070605040302010300000000105e5f02000000'), hashlib.sha256).digest()[:16].hex())"
    //   -> 5869d0d99c0a355d3a5e14402fed89aa
    const uint8_t whole[36] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x03, 0x00, 0x00, 0x00,
                               0x00, 0x10, 0x5E, 0x5F, 0x02, 0x00, 0x00, 0x00, 0x58, 0x69, 0xD0, 0xD9,
                               0x9C, 0x0A, 0x35, 0x5D, 0x3A, 0x5E, 0x14, 0x40, 0x2F, 0xED, 0x89, 0xAA};
    for (size_t i = 0; i < 36; ++i) CHECK(t[i] == whole[i]);
    auto back = checkTicket(testKey(), t.data(), t.size());
    REQUIRE(back.has_value());
    CHECK(back->sessionId == b.sessionId);
    CHECK(back->peerId == 3);
    CHECK(back->expiresAt == b.expiresAt);
    CHECK(back->role == kRoleJoiner);
}

TEST_CASE("a ticket with a changed byte, a wrong key or a wrong size is refused") {
    TicketBody b;
    b.sessionId = 9;
    b.peerId = 1;
    b.expiresAt = 1000;
    b.role = kRoleHost;
    TicketBytes t = makeTicket(testKey(), b);
    for (size_t i = 0; i < t.size(); ++i) {
        TicketBytes c = t;
        c[i] ^= 1;
        CAPTURE(i);
        CHECK_FALSE(checkTicket(testKey(), c.data(), c.size()).has_value());
    }
    TicketKey other = testKey();
    other[0] ^= 1;
    CHECK_FALSE(checkTicket(other, t.data(), t.size()).has_value());
    CHECK_FALSE(checkTicket(testKey(), t.data(), 35).has_value());
}

TEST_CASE("configuration is read from the environment and malformed values are named") {
    std::map<std::string, std::string> env;
    env["SFCOP_TICKET_KEY"] = std::string(62, '0') + "ff";
    env["SFCOP_RENDEZVOUS_HOST"] = "rv.example.test";
    env["SFCOP_RELAY_PORT"] = "50000";
    env["SFCOP_RELAY_HEALTH_PORT"] = "50001";
    env["SFCOP_BLOCKLIST"] = "198.51.100.7, 203.0.113.9";
    GetEnv get = [&](const char* n) -> const char* {
        auto it = env.find(n);
        return it == env.end() ? nullptr : it->second.c_str();
    };
    std::string error;
    auto c = loadConfig(get, error);
    REQUIRE(c.has_value());
    CHECK(c->ticketKey[31] == 0xff);
    CHECK(c->ticketKey[0] == 0);
    CHECK(c->rendezvousHost == "rv.example.test");
    CHECK(c->rendezvousPort == 50000);
    CHECK(c->relayHealthPort == 50001);
    CHECK(c->health.port == 47712); // the relay owns 47711
    CHECK(c->http.port == 8080);
    CHECK(c->blocked.count("203.0.113.9") == 1);

    env["SFCOP_RELAY_PORT"] = "70000";
    CHECK_FALSE(loadConfig(get, error).has_value());
    CHECK(error.find("SFCOP_RELAY_PORT") != std::string::npos);
    env["SFCOP_RELAY_PORT"] = "50000";
    env["SFCOP_BLOCKLIST"] = "not-an-address";
    CHECK_FALSE(loadConfig(get, error).has_value());
    CHECK(error.find("SFCOP_BLOCKLIST") != std::string::npos);
    env["SFCOP_BLOCKLIST"] = "";
    env["SFCOP_RENDEZVOUS_HOST"] = "lobby.example.test"; // the template's placeholder
    CHECK_FALSE(loadConfig(get, error).has_value());
    CHECK(error.find("SFCOP_RENDEZVOUS_HOST") != std::string::npos);
    CHECK(error.find("/etc/sfcop/lobby.env") != std::string::npos);
    env.erase("SFCOP_RENDEZVOUS_HOST");
    CHECK_FALSE(loadConfig(get, error).has_value());
    CHECK(error.find("SFCOP_RENDEZVOUS_HOST") != std::string::npos);
    env["SFCOP_RENDEZVOUS_HOST"] = "rv.example.test";
    std::string realKey = env["SFCOP_TICKET_KEY"];
    env["SFCOP_TICKET_KEY"] = std::string(64, '0');
    CHECK_FALSE(loadConfig(get, error).has_value());
    CHECK(error.find("all zeros") != std::string::npos);
    env["SFCOP_TICKET_KEY"] = realKey;
    env["SFCOP_HTTP_LISTEN"] = "127.0.0.1";
    CHECK_FALSE(loadConfig(get, error).has_value());
    CHECK(error.find("SFCOP_HTTP_LISTEN") != std::string::npos);
    env.erase("SFCOP_HTTP_LISTEN");
    env["SFCOP_TICKET_KEY"] = "abc";
    CHECK_FALSE(loadConfig(get, error).has_value());
    CHECK(error.find("SFCOP_TICKET_KEY") != std::string::npos);
}

TEST_CASE("a token bucket allows its burst, refuses the next and refills evenly") {
    RateLimiter rl;
    int retry = 0;
    for (int i = 0; i < 6; ++i) CHECK(rl.allow("a", 1, 6, 1000, &retry));
    CHECK_FALSE(rl.allow("a", 1, 6, 1000, &retry));
    CHECK(retry == 10); // 6 a minute is one token every 10 s
    CHECK_FALSE(rl.allow("a", 1, 6, 10999, &retry));
    CHECK(rl.allow("a", 1, 6, 11000, &retry));
    CHECK(rl.allow("b", 1, 6, 1000, &retry)); // another source has its own bucket
    CHECK(rl.allow("a", 2, 6, 1000, &retry)); // and another kind its own
}

TEST_CASE("idle sources are forgotten a minute after their last request") {
    RateLimiter rl;
    int retry = 0;
    rl.allow("a", 1, 6, 0, &retry);
    rl.allow("b", 1, 6, 30000, &retry);
    rl.sweep(59999);
    CHECK(rl.sourceCount() == 2);
    rl.sweep(60000);
    CHECK(rl.sourceCount() == 1);
    rl.sweep(90000);
    CHECK(rl.sourceCount() == 0);
}
