#include <doctest/doctest.h>

#include <memory>
#include <string>

#include "RelayService.h"
#include "TestSupport.h"

using namespace sfcop::relay;
using namespace sfcop::relay::test;

namespace {

const Endpoint kA = ep(192, 0, 2, 10, 40000);    // peer 1, the host
const Endpoint kB = ep(198, 51, 100, 20, 50000);  // peer 2
const Endpoint kC = ep(203, 0, 113, 30, 60000);   // a third machine
constexpr std::uint64_t kOther = 0x5346434f50000002ull;

Bytes fromHex(const std::string& hex) {
    Bytes b;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
        b.push_back(std::uint8_t(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return b;
}

RelayConfig defaultConfig() {
    RelayConfig c;
    c.ticketKey = testKey();
    return c;
}

struct Rig {
    FakeSender out;
    FakeTime time;
    LogCapture logs;
    std::uint32_t nextRandom = 0x1000;
    std::unique_ptr<RelayService> svc;

    explicit Rig(RelayConfig config = defaultConfig(), bool debug = false) {
        svc.reset(new RelayService(std::move(config), out, time, debug ? logs.debugLogger : logs.logger,
                                   [this] { return nextRandom++; }));
    }
    std::uint32_t expiry(int secondsFromNow = 600) const {
        return time.unix + std::uint32_t(secondsFromNow);
    }
    void send(const Endpoint& from, const Bytes& b) { svc->onDatagram(from, b.data(), b.size()); }
    void bind(std::uint32_t peer, const Endpoint& from, std::uint64_t session = kSession,
              std::vector<Candidate> locals = {}) {
        send(from, bindDatagram(makeTicket(session, peer, expiry()), 1, std::move(locals)));
    }
    // Binds the host (peer 1) at kA and peer 2 at kB, then forgets what was sent.
    void bindPair(std::uint64_t session = kSession) {
        bind(1, kA, session);
        bind(2, kB, session);
        out.clear();
    }
    std::uint32_t resultOfLast() const {
        const Bytes& d = out.sent.back().data;
        return std::uint32_t(d[12]) | (std::uint32_t(d[13]) << 8);
    }
    std::uint8_t typeOfLast() const { return out.sent.back().data[5]; }
};

}  // namespace

// ---- bind ---------------------------------------------------------------------------------------

TEST_CASE("a Bind with a ticket written out by hand is answered with the address the service saw") {
    Rig r;
    // Ticket: session 5346434f50000001, peer 1, expires 1800000600, role 1; its mac was computed
    // outside this code base with HMAC-SHA-256 over the key 00 01 02 ... 1f.
    Bytes bind = fromHex("53465256" "0101" "0000" "01000000"
                         "010000504f4346530100000058d4496b01000000"
                         "f07a187e257818e8c2922495362ea438"
                         "0000");
    r.send(kA, bind);
    REQUIRE(r.out.sent.size() == 1);
    CHECK(r.out.sent[0].to == kA);
    CHECK(r.out.sent[0].data ==
          fromHex("53465256" "0102" "0000" "01000000" "00000000" "0a0200c0" "409c" "02" "00" "0f00" "0000"));
    CHECK(r.svc->bindingCount() == 1);
}

TEST_CASE("a Bind with a wrong mac, a wrong key or an inconsistent role is refused as a bad ticket") {
    Rig r;
    Ticket t = makeTicket(kSession, 1, r.expiry());
    t.mac[3] ^= 1;
    r.send(kA, bindDatagram(t));
    CHECK(r.typeOfLast() == kRvError);
    CHECK(r.resultOfLast() == kBadTicket);

    Bytes other(32, 0x77);
    r.send(kA, bindDatagram(makeTicket(kSession, 1, r.expiry(), other)));
    CHECK(r.resultOfLast() == kBadTicket);

    Ticket role = makeTicket(kSession, 2, r.expiry());
    role.role = 1;  // a joiner's ticket claiming to be the host's; the mac no longer covers it
    r.send(kB, bindDatagram(role));
    CHECK(r.resultOfLast() == kBadTicket);
    CHECK(r.svc->bindingCount() == 0);
}

TEST_CASE("a new binding needs an unexpired ticket") {
    Rig r;
    r.send(kA, bindDatagram(makeTicket(kSession, 1, r.time.unix - 1)));
    CHECK(r.resultOfLast() == kTicketExpired);
    CHECK(r.svc->bindingCount() == 0);
}

TEST_CASE("a refresh from the binding's own address is accepted with an expired ticket") {
    Rig r;
    r.bind(1, kA);
    r.time.ms += 15000;
    r.time.unix += 700;  // the 10-minute ticket has run out
    r.out.clear();
    r.send(kA, bindDatagram(makeTicket(kSession, 1, r.time.unix - 100)));
    CHECK(r.typeOfLast() == kBindReply);
    CHECK(r.resultOfLast() == kOk);
}

TEST_CASE("a Bind from another address replaces the address only with an unexpired ticket") {
    Rig r;
    r.bind(1, kA);
    r.out.clear();
    r.send(kC, bindDatagram(makeTicket(kSession, 1, r.time.unix - 1)));
    CHECK(r.resultOfLast() == kTicketExpired);
    r.bind(2, kB);
    // Still bound at kA: an envelope from kC is refused and one from kA is forwarded.
    r.out.clear();
    r.send(kC, envelope(kSession, 1, 2, 10));
    CHECK(r.out.sent.empty());
    r.send(kA, envelope(kSession, 1, 2, 10));
    CHECK(r.out.sent.size() == 1);

    r.send(kC, bindDatagram(makeTicket(kSession, 1, r.expiry())));
    r.out.clear();
    r.send(kA, envelope(kSession, 1, 2, 10));
    CHECK(r.out.sent.empty());
    r.send(kC, envelope(kSession, 1, 2, 10));
    CHECK(r.out.sent.size() == 1);
}

TEST_CASE("a Bind carries its local candidates into the other peer's introduction, reflexive first") {
    Rig r;
    r.bind(1, kA, kSession, {{0xC0A80105, 40000, kLocal}, {0x0A000005, 40001, kReflexive}});
    r.bind(2, kB);
    r.out.clear();
    ConnectMessage c;
    c.header.transaction = 4;
    c.sessionId = kSession;
    c.source = 2;
    c.target = 1;
    r.send(kB, encodeConnect(c));
    REQUIRE(r.out.sent.size() == 2);
    // To B (the sender): A's reflexive address, then A's one local candidate; the candidate
    // that claimed to be reflexive is dropped.
    const Bytes& toB = r.out.sent[0].data;
    CHECK(r.out.sent[0].to == kB);
    CHECK(toB[28] == 2);  // candidateCount
    CHECK(Bytes(toB.begin() + 32, toB.begin() + 40) == fromHex("0a0200c0" "409c" "02" "00"));
    CHECK(Bytes(toB.begin() + 40, toB.begin() + 48) == fromHex("0501a8c0" "409c" "01" "00"));
}

TEST_CASE("a binding not refreshed for 45 seconds is gone, and a refresh at 44 seconds keeps it") {
    Rig r;
    r.bind(1, kA);
    r.time.ms += 44000;
    r.svc->tick();
    CHECK(r.svc->bindingCount() == 1);
    r.bind(1, kA);
    r.time.ms += 44000;
    r.svc->tick();
    CHECK(r.svc->bindingCount() == 1);
    r.time.ms += 1000;
    r.svc->tick();
    CHECK(r.svc->bindingCount() == 0);
}

TEST_CASE("Unbind drops a binding only when it comes from the binding's own address") {
    Rig r;
    r.bindPair();
    UnbindMessage u;
    u.sessionId = kSession;
    u.peer = 1;
    r.send(kC, encodeUnbind(u));
    CHECK(r.svc->bindingCount() == 2);
    r.send(kA, encodeUnbind(u));
    CHECK(r.svc->bindingCount() == 1);
    CHECK(r.out.sent.empty());  // no reply either way
}

TEST_CASE("a full table answers kFull and a refresh of a held binding still works") {
    RelayConfig c = defaultConfig();
    c.maxBindings = 2;
    Rig r(c);
    r.bindPair();
    r.send(kC, bindDatagram(makeTicket(kSession, 3, r.expiry())));
    CHECK(r.resultOfLast() == kFull);
    CHECK(r.svc->bindingCount() == 2);
    r.bind(1, kA);
    CHECK(r.typeOfLast() == kBindReply);
}

TEST_CASE("a Bind with a newer protocol version is answered kUnsupportedVersion") {
    Rig r;
    Bytes b = bindDatagram(makeTicket(kSession, 1, r.expiry()));
    b[4] = 2;
    r.send(kA, b);
    CHECK(r.typeOfLast() == kRvError);
    CHECK(r.resultOfLast() == kUnsupportedVersion);
    CHECK(r.svc->bindingCount() == 0);
}

TEST_CASE("malformed rendezvous datagrams are dropped without a reply") {
    Rig r;
    Bytes bind = bindDatagram(makeTicket(kSession, 1, r.expiry()));
    bind.push_back(0);  // trailing byte
    r.send(kA, bind);
    Bytes shortHeader = {0x53, 0x46, 0x52, 0x56, 1, 1};
    r.send(kA, shortHeader);
    Bytes junk = {1, 2, 3, 4, 5, 6, 7, 8};
    r.send(kA, junk);
    Bytes tiny = {1, 2};
    r.send(kA, tiny);
    CHECK(r.out.sent.empty());
    CHECK(r.svc->stats().refusedMalformed == 2);
    CHECK(r.svc->stats().refusedUnknownKind == 2);
}

// ---- connect ------------------------------------------------------------------------------------

TEST_CASE("Connect introduces both peers with the same nonzero punch id, twice, 100 ms apart") {
    Rig r;
    r.bindPair();
    ConnectMessage c;
    c.header.transaction = 0x44;
    c.sessionId = kSession;
    c.source = 2;
    c.target = 1;
    r.send(kB, encodeConnect(c));
    REQUIRE(r.out.sent.size() == 2);
    const Sent toB = r.out.sent[0];
    const Sent toA = r.out.sent[1];
    CHECK(toB.to == kB);
    CHECK(toA.to == kA);
    // To B: header type 4 with B's transaction, session, peer 1, punch id 0x1000, one candidate.
    CHECK(toB.data == fromHex("53465256" "0104" "0000" "44000000" "010000504f434653" "01000000"
                              "00100000" "01" "000000" "0a0200c0" "409c" "02" "00"));
    // To A: transaction 0, peer 2, the same punch id, B's reflexive candidate.
    CHECK(toA.data == fromHex("53465256" "0104" "0000" "00000000" "010000504f434653" "02000000"
                              "00100000" "01" "000000" "146433c6" "50c3" "02" "00"));
    // (198.51.100.20 is 14 64 33 c6 on the wire; port 50000 is 50 c3.)
    r.out.clear();
    r.time.ms += 99;
    r.svc->tick();
    CHECK(r.out.sent.empty());
    r.time.ms += 1;
    r.svc->tick();
    REQUIRE(r.out.sent.size() == 2);
    CHECK(r.out.sent[0].data == toB.data);
    CHECK(r.out.sent[1].data == toA.data);
    r.out.clear();
    r.time.ms += 1000;
    r.svc->tick();
    CHECK(r.out.sent.empty());
}

TEST_CASE("a repeated Connect within three seconds reuses the punch id, and the reverse direction too") {
    Rig r;
    r.bindPair();
    ConnectMessage c;
    c.sessionId = kSession;
    c.source = 2;
    c.target = 1;
    r.send(kB, encodeConnect(c));
    Bytes first = r.out.sent[0].data;
    r.out.clear();
    r.time.ms += 250;
    r.send(kB, encodeConnect(c));
    CHECK(r.out.sent[0].data == first);
    r.out.clear();
    c.source = 1;
    c.target = 2;
    r.send(kA, encodeConnect(c));
    CHECK(Bytes(r.out.sent[1].data.begin() + 24, r.out.sent[1].data.begin() + 28) ==
          Bytes(first.begin() + 24, first.begin() + 28));
    r.out.clear();
    r.time.ms += 5000;
    r.send(kA, encodeConnect(c));
    CHECK(Bytes(r.out.sent[1].data.begin() + 24, r.out.sent[1].data.begin() + 28) !=
          Bytes(first.begin() + 24, first.begin() + 28));
}

TEST_CASE("Connect from a machine that is not bound at that address is answered kUnknownBinding, and one for no session at all kUnknownSession") {
    Rig r;
    r.bindPair();
    ConnectMessage c;
    c.sessionId = kSession;
    c.source = 2;
    c.target = 1;
    r.send(kC, encodeConnect(c));
    REQUIRE(r.out.sent.size() == 1);
    CHECK(r.out.sent[0].to == kC);
    CHECK(r.resultOfLast() == kUnknownBinding);
    // A session with no bindings at all is a different answer: the transport maps it to not found.
    c.sessionId = kOther;
    r.send(kB, encodeConnect(c));
    CHECK(r.resultOfLast() == kUnknownSession);
}

TEST_CASE("Connect to a peer that is not bound, or to oneself, is answered kUnknownPeer") {
    Rig r;
    r.bindPair();
    ConnectMessage c;
    c.sessionId = kSession;
    c.source = 2;
    c.target = 9;
    r.send(kB, encodeConnect(c));
    CHECK(r.resultOfLast() == kUnknownPeer);
    c.target = 2;
    r.send(kB, encodeConnect(c));
    CHECK(r.resultOfLast() == kUnknownPeer);
}

// ---- relay --------------------------------------------------------------------------------------

TEST_CASE("the relay forwards an envelope byte for byte, in both directions") {
    Rig r;
    r.bindPair();
    Bytes toB = envelope(kSession, 1, 2, 300, 0x5A);
    r.send(kA, toB);
    REQUIRE(r.out.sent.size() == 1);
    CHECK(r.out.sent[0].to == kB);
    CHECK(r.out.sent[0].data == toB);

    Bytes toA = envelope(kSession, 2, 1, 1168, 0xC3);  // the largest legal datagram, 1200 bytes
    REQUIRE(toA.size() == 1200);
    r.send(kB, toA);
    REQUIRE(r.out.sent.size() == 2);
    CHECK(r.out.sent[1].to == kA);
    CHECK(r.out.sent[1].data == toA);
    CHECK(r.svc->stats().forwardedDatagrams == 2);
    CHECK(r.svc->stats().forwardedBytes == 332 + 1200);
}

TEST_CASE("an envelope from a third address with a valid session and peer ids is not forwarded") {
    Rig r;
    r.bindPair();
    r.send(kC, envelope(kSession, 1, 2, 10));  // pretends to be peer 1
    r.send(kC, envelope(kSession, 3, 2, 10));  // claims a peer that is not bound
    CHECK(r.out.sent.empty());
    CHECK(r.svc->stats().refusedNotBound == 2);
}

TEST_CASE("a bound machine cannot relay into another session or to a peer that is not bound") {
    Rig r;
    r.bindPair();
    r.bind(1, kC, kOther);
    r.out.clear();
    r.send(kA, envelope(kOther, 1, 2, 10));     // kA is not bound in the other session
    r.send(kC, envelope(kOther, 1, 2, 10));     // peer 2 is not bound there
    r.send(kA, envelope(kSession, 1, 7, 10));   // no such destination
    r.send(kA, envelope(kSession, 1, 1, 10));   // to itself
    CHECK(r.out.sent.empty());
    CHECK(r.svc->stats().refusedNotBound == 4);
}

TEST_CASE("an envelope is forwarded only while both bindings are live; idle sessions time out") {
    Rig r;
    r.bindPair();
    r.time.ms += 30000;
    r.bind(1, kA);  // the host refreshes, the other peer does not
    r.out.clear();
    r.time.ms += 20000;  // B is now 50 s idle
    r.send(kA, envelope(kSession, 1, 2, 10));
    CHECK(r.out.sent.empty());
    r.time.ms += 20000;  // A is idle 40 s: alive; the table forgot B
    r.svc->tick();
    CHECK(r.svc->bindingCount() == 1);
    r.time.ms += 10000;
    r.svc->tick();
    CHECK(r.svc->bindingCount() == 0);
}

TEST_CASE("oversized, short, unknown-version and unknown-magic datagrams are dropped silently") {
    Rig r;
    r.bindPair();
    r.send(kA, envelope(kSession, 1, 2, 1169));  // 1201 bytes
    Bytes shortEnvelope = envelope(kSession, 1, 2, 0);
    shortEnvelope.pop_back();
    r.send(kA, shortEnvelope);
    r.send(kA, envelope(kSession, 1, 2, 10, 0xAB, 2));
    Bytes alien = envelope(kSession, 1, 2, 10);
    alien[0] = 0x58;
    r.send(kA, alien);
    CHECK(r.out.sent.empty());
    CHECK(r.svc->stats().refusedBadEnvelope == 3);
    CHECK(r.svc->stats().refusedUnknownKind == 1);
    // And the 1200-byte limit is inclusive.
    r.send(kA, envelope(kSession, 1, 2, 1168));
    CHECK(r.out.sent.size() == 1);
}

TEST_CASE("the relay never sends an error back for a refused envelope") {
    Rig r;
    r.bindPair();
    r.send(kC, envelope(kSession, 1, 2, 10));
    r.send(kA, envelope(kOther, 1, 2, 10));
    CHECK(r.out.sent.empty());
}

TEST_CASE("removeSession drops that session's bindings and no others") {
    Rig r;
    r.bindPair();
    r.bind(1, kC, kOther);
    CHECK(r.svc->bindingCount() == 3);
    CHECK(r.svc->removeSession(kSession) == 2);
    CHECK(r.svc->bindingCount() == 1);
    r.out.clear();
    r.send(kA, envelope(kSession, 1, 2, 10));
    CHECK(r.out.sent.empty());
    CHECK(r.svc->removeSession(kSession) == 0);
}

// ---- limits -------------------------------------------------------------------------------------

TEST_CASE("one binding is held to 1000 datagrams a second with a one-second burst") {
    Rig r;
    r.bindPair();
    for (int i = 0; i < 5000; ++i) r.send(kA, envelope(kSession, 1, 2, 20));
    CHECK(r.out.sent.size() == 1000);
    CHECK(r.svc->stats().rateLimitedRelay == 4000);
    r.out.clear();
    r.time.ms += 500;
    for (int i = 0; i < 5000; ++i) r.send(kA, envelope(kSession, 1, 2, 20));
    CHECK(r.out.sent.size() == 500);
}

TEST_CASE("one binding is held to 512 KB a second") {
    Rig r;
    r.bindPair();
    for (int i = 0; i < 1000; ++i) r.send(kA, envelope(kSession, 1, 2, 1168));
    // 524288 / 1200 = 436 whole datagrams.
    CHECK(r.out.sent.size() == 436);
    r.out.clear();
    r.time.ms += 1000;
    for (int i = 0; i < 1000; ++i) r.send(kA, envelope(kSession, 1, 2, 1168));
    CHECK(r.out.sent.size() == 436);
}

TEST_CASE("a session that floods does not slow another session or the same session's other peer") {
    Rig r;
    r.bindPair();
    r.bind(1, kC, kOther);
    r.bind(2, ep(203, 0, 113, 31, 61000), kOther);
    r.out.clear();
    for (int i = 0; i < 3000; ++i) r.send(kA, envelope(kSession, 1, 2, 20));
    std::size_t flooded = r.out.sent.size();
    CHECK(flooded == 1000);
    // The other session, and peer 2 of the flooded session, still get their full allowance.
    for (int i = 0; i < 1000; ++i) r.send(kC, envelope(kOther, 1, 2, 20));
    CHECK(r.out.sent.size() == flooded + 1000);
    for (int i = 0; i < 1000; ++i) r.send(kB, envelope(kSession, 2, 1, 20));
    CHECK(r.out.sent.size() == flooded + 2000);
}

TEST_CASE("Bind is limited to 30 a minute per source and the 31st is answered kRateLimited") {
    Rig r;
    for (int i = 0; i < 30; ++i) r.bind(1, kA);
    CHECK(r.typeOfLast() == kBindReply);
    r.out.clear();
    r.bind(1, kA);
    CHECK(r.typeOfLast() == kRvError);
    CHECK(r.resultOfLast() == kRateLimited);
    // Another source is unaffected, and the allowance comes back with time.
    r.bind(2, kB);
    CHECK(r.typeOfLast() == kBindReply);
    r.time.ms += 2000;
    r.bind(1, kA);
    CHECK(r.typeOfLast() == kBindReply);
}

TEST_CASE("all rendezvous messages together are limited to 120 a minute, and past it nothing is answered") {
    Rig r;
    r.bindPair();
    ConnectMessage c;
    c.sessionId = kSession;
    c.source = 2;
    c.target = 1;
    Bytes connect = encodeConnect(c);
    // The 2 binds above used 1 token each of B's 120 (kB bound once).
    int answered = 0;
    for (int i = 0; i < 200; ++i) {
        r.out.clear();
        r.send(kB, connect);
        if (!r.out.sent.empty()) ++answered;
    }
    // 119 left; Connect's own limit of 60 comes first and answers kRateLimited up to the total.
    CHECK(answered == 119);
    r.out.clear();
    r.send(kB, connect);
    CHECK(r.out.sent.empty());
}

TEST_CASE("an address on the block list is dropped unanswered, relay and rendezvous alike") {
    Rig r;
    r.bindPair();
    r.svc->setBlocklist({kA.address});
    r.send(kA, envelope(kSession, 1, 2, 10));
    r.send(kA, bindDatagram(makeTicket(kSession, 1, r.expiry())));
    CHECK(r.out.sent.empty());
    CHECK(r.svc->stats().refusedBlocked == 2);
    r.svc->setBlocklist({});
    r.send(kA, envelope(kSession, 1, 2, 10));
    CHECK(r.out.sent.size() == 1);
}

// ---- privacy ------------------------------------------------------------------------------------

TEST_CASE("nothing identifying is logged above debug level over a session's whole life") {
    Rig r;
    r.bindPair();
    ConnectMessage c;
    c.sessionId = kSession;
    c.source = 2;
    c.target = 1;
    r.send(kB, encodeConnect(c));
    r.send(kA, envelope(kSession, 1, 2, 10));
    r.send(kC, envelope(kSession, 1, 2, 10));              // refused
    r.send(kC, bindDatagram(makeTicket(kSession, 1, 1)));  // refused, expired
    r.svc->setBlocklist({kC.address});
    r.send(kC, envelope(kSession, 1, 2, 10));              // blocked
    for (int i = 0; i < 40; ++i) r.bind(1, kA);            // rate limited
    for (int i = 0; i < 4; ++i) {
        r.time.ms += 20000;
        r.svc->tick();
    }
    r.svc->removeSession(kSession);
    r.time.ms += 61000;
    r.svc->tick();
    REQUIRE_FALSE(r.logs.lines.empty());  // the minute report
    for (const auto& line : r.logs.lines) {
        const std::string& s = line.second;
        CHECK(s.find("192.0.2") == std::string::npos);
        CHECK(s.find("198.51.100") == std::string::npos);
        CHECK(s.find("203.0.113") == std::string::npos);
        CHECK(s.find("5346434f50000001") == std::string::npos);
        CHECK(s.find("40000") == std::string::npos);
        CHECK(line.first != LogLevel::kDebug);
    }
}

TEST_CASE("the same events are traceable at debug level, where addresses may appear") {
    Rig r(defaultConfig(), true);
    r.bindPair();
    r.send(kC, bindDatagram(makeTicket(kSession, 1, 1)));
    r.svc->setBlocklist({kC.address});
    r.send(kC, envelope(kSession, 1, 2, 10));
    bool sawAddress = false;
    for (const auto& line : r.logs.lines)
        if (line.second.find("203.0.113.30") != std::string::npos) sawAddress = true;
    CHECK(sawAddress);
}

// ---- review round 1 -----------------------------------------------------------------------------

TEST_CASE("a valid mac does not rescue a role that disagrees with the peer id") {
    Rig r;
    r.send(kB, bindDatagram(makeTicket(kSession, 2, r.expiry(), testKey(), 1)));
    CHECK(r.resultOfLast() == kBadTicket);
    r.send(kA, bindDatagram(makeTicket(kSession, 1, r.expiry(), testKey(), 2)));
    CHECK(r.resultOfLast() == kBadTicket);
    CHECK(r.svc->bindingCount() == 0);
}

TEST_CASE("the rendezvous limits are per IPv4 address, so rotating ports gains nothing") {
    Rig r;
    for (int i = 0; i < 30; ++i) r.send(ep(192, 0, 2, 10, std::uint16_t(41000 + i)),
                                        bindDatagram(makeTicket(kSession, 1, r.expiry())));
    r.out.clear();
    r.send(ep(192, 0, 2, 10, 49999), bindDatagram(makeTicket(kSession, 1, r.expiry())));
    CHECK(r.resultOfLast() == kRateLimited);
    CHECK(r.svc->trackedSources() == 1);
}

TEST_CASE("a full source table evicts the address idle the longest and a new address still binds") {
    RelayConfig c = defaultConfig();
    c.maxTrackedSources = 3;
    Rig r(c);
    for (int i = 1; i <= 3; ++i) {  // three addresses fill the table, each idle a second longer
        r.send(ep(192, 0, 2, std::uint8_t(i), 4000), bindDatagram(makeTicket(kSession, 5, r.expiry())));
        r.time.ms += 1000;
    }
    CHECK(r.svc->trackedSources() == 3);
    r.out.clear();
    r.bind(2, kB);  // a fourth address
    CHECK(r.typeOfLast() == kBindReply);
    CHECK(r.svc->trackedSources() == 3);
    // 192.0.2.1 was the idle one; it starts over with a full allowance rather than being refused.
    r.out.clear();
    r.send(ep(192, 0, 2, 1, 4000), bindDatagram(makeTicket(kSession, 5, r.expiry())));
    CHECK(r.typeOfLast() == kBindReply);
}

TEST_CASE("an unsupported version is answered for Bind and Connect and for nothing else") {
    Rig r;
    ConnectMessage c;
    c.sessionId = kSession;
    c.source = 2;
    c.target = 1;
    Bytes connect = encodeConnect(c);
    connect[4] = 2;
    r.send(kB, connect);
    CHECK(r.resultOfLast() == kUnsupportedVersion);
    r.out.clear();
    UnbindMessage u;
    u.sessionId = kSession;
    u.peer = 1;
    Bytes unbind = encodeUnbind(u);
    unbind[4] = 2;
    r.send(kB, unbind);
    Bytes probe = {0x53, 0x46, 0x52, 0x56, 2, 5, 0, 0, 1, 0, 0, 0};
    r.send(kB, probe);
    probe[5] = 8;
    r.send(kB, probe);
    CHECK(r.out.sent.empty());
}

TEST_CASE("a Bind whose candidates are unusable addresses is dropped, loopback is allowed") {
    Rig r;
    const std::uint32_t bad[] = {0x00000000u, 0xFFFFFFFFu, 0xE0000001u, 0xEFFFFFFFu};
    for (std::uint32_t a : bad) {
        r.send(kA, bindDatagram(makeTicket(kSession, 1, r.expiry()), 1, {{a, 4000, kLocal}}));
    }
    r.send(kA, bindDatagram(makeTicket(kSession, 1, r.expiry()), 1, {{0xC0A80105u, 0, kLocal}}));
    CHECK(r.out.sent.empty());
    CHECK(r.svc->bindingCount() == 0);
    r.send(kA, bindDatagram(makeTicket(kSession, 1, r.expiry()), 1, {{0x7F000001u, 4000, kLocal}}));
    CHECK(r.typeOfLast() == kBindReply);
    CHECK(r.svc->bindingCount() == 1);
}

TEST_CASE("unbinding twice, removing a session twice and binding again keep the count exact") {
    Rig r;
    r.bindPair();
    r.bind(2, kB);  // a refresh from the same address: still one entry each
    CHECK(r.svc->bindingCount() == 2);
    UnbindMessage u;
    u.sessionId = kSession;
    u.peer = 2;
    r.send(kB, encodeUnbind(u));
    r.send(kB, encodeUnbind(u));
    CHECK(r.svc->bindingCount() == 1);
    CHECK(r.svc->removeSession(kSession) == 1);
    CHECK(r.svc->removeSession(kSession) == 0);
    CHECK(r.svc->bindingCount() == 0);
    r.svc->tick();
    CHECK(r.svc->bindingCount() == 0);
}

TEST_CASE("the lobby's published ticket vector is accepted, and refused with one body byte changed") {
    // Key 00 01 02 ... 1f. Body: session 0x0102030405060708, peer 3, expires 1600000000 (0x5f5e1000),
    // role 2. Mac: the first 16 bytes of HMAC-SHA-256 over the body, as the lobby computes it.
    Rig r;
    r.time.unix = 1599999000;
    const std::string body = "08070605040302010300000000105e5f02000000";
    const std::string mac = "5869d0d99c0a355d3a5e14402fed89aa";
    const Endpoint from = ep(198, 51, 100, 7, 41000);

    r.send(from, fromHex("53465256" "0101" "0000" "01000000" + body + mac + "0000"));
    REQUIRE(r.out.sent.size() == 1);
    CHECK(r.typeOfLast() == kBindReply);
    CHECK(r.svc->bindingCount() == 1);

    // Peer id 3 -> 4 in the body, mac unchanged.
    std::string changed = body;
    changed[16] = '0';
    changed[17] = '4';
    r.send(ep(198, 51, 100, 8, 41000), fromHex("53465256" "0101" "0000" "02000000" + changed + mac + "0000"));
    CHECK(r.typeOfLast() == kRvError);
    CHECK(r.resultOfLast() == kBadTicket);
    CHECK(r.svc->bindingCount() == 1);
}
