// Fakes for the tests: a recording Sender, a settable clock, a log that keeps every line, and
// helpers that build tickets and datagrams. Addresses are from RFC 5737; keys are made up.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "RelayService.h"
#include "Sha256.h"

namespace sfcop::relay::test {

struct Sent {
    Endpoint to;
    Bytes data;
};

class FakeSender : public Sender {
public:
    void send(const Endpoint& to, const std::uint8_t* data, std::size_t size) override {
        sent.push_back({to, Bytes(data, data + size)});
    }
    std::vector<Sent> sent;
    void clear() { sent.clear(); }
};

class FakeTime : public TimeSource {
public:
    std::uint64_t nowMs() const override { return ms; }
    std::uint32_t unixSeconds() const override { return unix; }
    std::uint64_t ms = 1000000;
    std::uint32_t unix = 1800000000;
};

struct LogCapture {
    std::vector<std::pair<LogLevel, std::string>> lines;
    Logger logger{LogLevel::kInfo, [this](LogLevel l, const std::string& s) { lines.push_back({l, s}); }};
    Logger debugLogger{LogLevel::kDebug, [this](LogLevel l, const std::string& s) { lines.push_back({l, s}); }};
};

inline Bytes testKey() {
    Bytes k;
    for (int i = 0; i < 32; ++i) k.push_back(std::uint8_t(i));
    return k;
}

inline Endpoint ep(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d, std::uint16_t port) {
    return {(std::uint32_t(a) << 24) | (std::uint32_t(b) << 16) | (std::uint32_t(c) << 8) | d, port};
}

constexpr std::uint64_t kSession = 0x5346434f50000001ull;

inline Ticket makeTicket(std::uint64_t session, std::uint32_t peer, std::uint32_t expiresAt,
                         const Bytes& key = testKey(), int role = 0) {
    Ticket t;
    t.sessionId = session;
    t.peerId = peer;
    t.expiresAt = expiresAt;
    t.role = role ? std::uint8_t(role) : (peer == 1 ? 1 : 2);
    std::uint8_t body[20];
    encodeTicketBody(t, body);
    Digest d = hmacSha256(key.data(), key.size(), body, sizeof body);
    for (int i = 0; i < 16; ++i) t.mac[i] = d[std::size_t(i)];
    return t;
}

inline Bytes bindDatagram(const Ticket& t, std::uint32_t txn = 1,
                          std::vector<Candidate> locals = {}) {
    BindMessage m;
    m.header.transaction = txn;
    m.ticket = t;
    m.candidates = std::move(locals);
    return encodeBind(m);
}

// A well-formed envelope header (PROTOCOL.md) followed by `payload` filler bytes.
inline Bytes envelope(std::uint64_t session, std::uint32_t source, std::uint32_t destination,
                      std::size_t payload, std::uint8_t fill = 0xAB, std::uint8_t version = 1) {
    Bytes b(32 + payload, fill);
    auto put = [&](std::size_t off, std::uint64_t v, int n) {
        for (int i = 0; i < n; ++i) b[off + std::size_t(i)] = std::uint8_t(v >> (8 * i));
    };
    put(0, 0x454E4653, 4);
    b[4] = version;
    b[5] = 1;
    b[6] = 0;
    b[7] = 0;
    put(8, session, 8);
    put(16, source, 4);
    put(20, destination, 4);
    put(24, 7, 4);
    put(28, 0, 2);
    put(30, 1, 2);
    return b;
}

}  // namespace sfcop::relay::test
