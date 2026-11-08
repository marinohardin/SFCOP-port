// The lobby's call to the relay (RelayClient.h) against a test double that stands in for the
// relay's loopback endpoint: it answers a canned response and records the request. No sockets.
#include "Log.h"
#include "Ticket.h"
#include "Lobby.h"
#include "RelayClient.h"

#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <chrono>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace sfcop::lobby;

namespace {
struct Double {
    std::vector<std::string> requests;
    std::optional<std::string> answer;
    RelayTransport transport() {
        return [this](const std::string& r) -> std::optional<std::string> {
            requests.push_back(r);
            return answer;
        };
    }
};
} // namespace

TEST_CASE("the removal request is a bodiless DELETE of the session's 16 hex digits") {
    CHECK(relayRemovalRequest(0x0102030405060708ull) ==
          "DELETE /v1/bindings/0102030405060708 HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
    CHECK(relayRemovalRequest(0xABCDEF) ==
          "DELETE /v1/bindings/0000000000abcdef HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
}

TEST_CASE("the status of an answer is read from its first line") {
    CHECK(httpStatusOf("HTTP/1.1 204 No Content\r\n\r\n") == 204);
    CHECK(httpStatusOf("HTTP/1.1 400 Bad Request\r\n\r\n") == 400);
    CHECK(httpStatusOf("garbage") == 0);
    CHECK(httpStatusOf("HTTP/1.1 2x4") == 0);
}

TEST_CASE("a removal the relay answers 204 is sent once and logs nothing above debug") {
    std::vector<LogLevel> levels;
    log::setSink([&](LogLevel l, const std::string&) { levels.push_back(l); });
    Double relay;
    relay.answer = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
    HttpRelayClient client(relay.transport());
    client.removeSession(0x1111111111111111ull);
    REQUIRE(relay.requests.size() == 1);
    CHECK(relay.requests[0].rfind("DELETE /v1/bindings/1111111111111111 ", 0) == 0);
    CHECK(levels.empty());
    log::setSink([](LogLevel, const std::string&) {});
}

TEST_CASE("a 400 from the relay is logged at debug and the lobby carries on") {
    std::vector<std::pair<LogLevel, std::string>> lines;
    log::setSink([&](LogLevel l, const std::string& s) { lines.emplace_back(l, s); });
    Double relay;
    relay.answer = "HTTP/1.1 400 Bad Request\r\n\r\n";
    HttpRelayClient client(relay.transport());
    client.removeSession(1);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].first == LogLevel::Debug);
    CHECK(lines[0].second == "relay-client: the relay answered 400 to a removal");
    log::setSink([](LogLevel, const std::string&) {});
}

TEST_CASE("a relay that cannot be reached is logged at debug and never blocks removal in the lobby") {
    std::vector<std::pair<LogLevel, std::string>> lines;
    log::setSink([&](LogLevel l, const std::string& s) { lines.emplace_back(l, s); });
    Double relay; // answer stays empty: connection refused
    HttpRelayClient client(relay.transport());

    LobbyConfig cfg;
    uint8_t counter = 0;
    Lobby lobby(cfg, [&](uint8_t* o, size_t n) { ++counter; for (size_t i = 0; i < n; ++i) o[i] = counter; }, 1700000000000ull);
    lobby.setRelayClient(&client);
    HttpRequest reg;
    reg.method = "POST";
    reg.path = "/v1/sessions";
    reg.source = "198.51.100.7";
    reg.body = R"({"protocol":1,"application":"{0AB1C531-4745-11D1-A7A1-0000F803ABFC}","name":"G","flags":0,"max_players":4,"current_players":1,"user":[0,0,0,0],"password_required":false})";
    CHECK(lobby.handle(reg, 1700000000000ull).status == 201);
    CHECK(lobby.sessionCount() == 1);
    lobby.sweep(1700000000000ull + 45000); // expires; the relay call fails
    CHECK(lobby.sessionCount() == 0);      // removal went through regardless
    REQUIRE(relay.requests.size() == 1);
    CHECK(relay.requests[0].rfind("DELETE /v1/bindings/0101010101010101 ", 0) == 0);
    bool sawDebug = false;
    for (const auto& l : lines) sawDebug |= (l.first == LogLevel::Debug && l.second.rfind("relay-client:", 0) == 0);
    CHECK(sawDebug);
    log::setSink([](LogLevel, const std::string&) {});
}

TEST_CASE("tickets follow the relay's rule: role 1 exactly when the peer id is 1") {
    // LobbyTest checks the issued tickets (peer 1 role 1, peers 2 and up role 2). Here the same
    // hand-written vector N4 pins: key bytes 0x00..0x1f, body 08070605040302010300000000105e5f02000000
    // (session 0x0102030405060708, peer 3, expiry 0x5F5E1000, role 2), mac = first 16 bytes of
    // HMAC-SHA-256, computed once with python3 hmac: 5869d0d99c0a355d3a5e14402fed89aa.
    TicketKey k{};
    for (size_t i = 0; i < k.size(); ++i) k[i] = uint8_t(i);
    TicketBody b;
    b.sessionId = 0x0102030405060708ull;
    b.peerId = 3;
    b.expiresAt = 0x5F5E1000;
    b.role = kRoleJoiner;
    TicketBytes t = makeTicket(k, b);
    const uint8_t whole[36] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x03, 0x00, 0x00, 0x00,
                               0x00, 0x10, 0x5E, 0x5F, 0x02, 0x00, 0x00, 0x00, 0x58, 0x69, 0xD0, 0xD9,
                               0x9C, 0x0A, 0x35, 0x5D, 0x3A, 0x5E, 0x14, 0x40, 0x2F, 0xED, 0x89, 0xAA};
    for (size_t i = 0; i < 36; ++i) CHECK(t[i] == whole[i]);
}

namespace {
// A real listener on an ephemeral loopback port, in this process: it answers the first connection
// with `answer` at once, then keeps the connection open until the client closes it, and records
// the request it read.
struct LoopbackRelay {
    int listener = -1;
    uint16_t port = 0;
    std::string request;
    std::thread worker;

    explicit LoopbackRelay(const std::string& answer) {
        listener = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
        bind(listener, reinterpret_cast<sockaddr*>(&sa), sizeof sa);
        ::listen(listener, 1);
        socklen_t len = sizeof sa;
        getsockname(listener, reinterpret_cast<sockaddr*>(&sa), &len);
        port = ntohs(sa.sin_port);
        worker = std::thread([this, answer] {
            int c = accept(listener, nullptr, nullptr);
            if (c < 0) return;
            char buf[512];
            ssize_t n = recv(c, buf, sizeof buf, 0);
            if (n > 0) request.assign(buf, size_t(n));
            send(c, answer.data(), answer.size(), 0);
            while (recv(c, buf, sizeof buf, 0) > 0) {} // held open until the client closes
            close(c);
        });
    }
    ~LoopbackRelay() {
        shutdown(listener, SHUT_RDWR);
        close(listener);
        if (worker.joinable()) worker.join();
    }
};

uint16_t freePortNobodyListensOn() {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    bind(s, reinterpret_cast<sockaddr*>(&sa), sizeof sa);
    socklen_t len = sizeof sa;
    getsockname(s, reinterpret_cast<sockaddr*>(&sa), &len);
    close(s);
    return ntohs(sa.sin_port);
}
} // namespace

TEST_CASE("against a real loopback listener: 204 is read from the status line without waiting for close") {
    LoopbackRelay relay("HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n");
    auto transport = loopbackTransport(relay.port);
    auto start = std::chrono::steady_clock::now();
    auto answer = transport(relayRemovalRequest(0x0102030405060708ull));
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    REQUIRE(answer.has_value());
    CHECK(httpStatusOf(*answer) == 204);
    CHECK(elapsedMs < 90); // the listener holds the connection open; the client did not wait for it
    // closing our side lets the worker finish; its request is what the relay would have read
    CHECK(relay.request.rfind("DELETE /v1/bindings/0102030405060708 HTTP/1.1\r\n", 0) == 0);
}

TEST_CASE("against a real loopback listener: a 400 is read as a 400") {
    LoopbackRelay relay("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n");
    auto answer = loopbackTransport(relay.port)(relayRemovalRequest(1));
    REQUIRE(answer.has_value());
    CHECK(httpStatusOf(*answer) == 400);
}

TEST_CASE("against a port nobody listens on: no answer, at once") {
    auto start = std::chrono::steady_clock::now();
    auto answer = loopbackTransport(freePortNobodyListensOn())(relayRemovalRequest(1));
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    CHECK_FALSE(answer.has_value());
    CHECK(elapsedMs < 90);
}
