// IDirectPlay4A connection and session methods on TransportMock: the datagrams written out by
// hand, both those the object sends and those a host at 192.0.2.10:2300 answers with.
#include "hle/dplay/DirectPlay.h"

#include "hle/dplay/DplayMockFixture.h"

#include <doctest/doctest.h>

#include <algorithm>

namespace sfcop::hle::dplay {

using namespace test;

namespace {

struct Mocked {
    platform::TransportMock transport;
    FakeClock clock;
    std::unique_ptr<DirectPlay> dp;

    explicit Mocked(Config config = Lan::testConfig()) {
        Environment env;
        env.transport = &transport;
        env.clock = &clock;
        dp = std::make_unique<DirectPlay>(env, config);
    }
};

const std::vector<std::uint8_t> kEnumRequest1 = concat(
    {hex("53 46 44 50 01 01 00 00 01 00 00 00"), guidBytes(testApplication())});

std::vector<std::uint8_t> joinRequest(const char *password) {
    return concat({hex("53 46 44 50 01 03 00 00"), guidBytes(mockInstance()), hex(password)});
}

SessionDesc applicationFilter() {
    SessionDesc filter;
    filter.application = testApplication();
    return filter;
}

} // namespace

TEST_CASE("EnumConnections offers the TCP/IP connection only") {
    Mocked m;
    std::vector<Connection> seen;
    auto collect = [&](const Connection &c) {
        seen.push_back(c);
        return true;
    };
    REQUIRE(m.dp->EnumConnections(nullptr, collect, 0) == kDpOk);
    REQUIRE(seen.size() == 1u);
    CHECK(seen[0].serviceProvider == kSpTcpIp);
    CHECK(seen[0].address == tcpIpConnection());
    CHECK(seen[0].name.shortName == std::optional<std::string>("Internet TCP/IP Connection"));
    CHECK(seen[0].flags == kConnectionDirectPlay);
    seen.clear();
    REQUIRE(m.dp->EnumConnections(nullptr, collect, 2) == kDpOk); // lobby connections
    CHECK(seen.empty());
    CHECK(m.dp->EnumConnections(nullptr, nullptr, 0) == kDpErrInvalidParams);
}

TEST_CASE("InitializeConnection takes a TCP/IP address once") {
    Mocked m;
    std::vector<std::uint8_t> connection = tcpIpConnection();
    CHECK(m.dp->InitializeConnection(connection.data(), connection.size(), 1) ==
          kDpErrInvalidFlags);
    CHECK(m.dp->InitializeConnection(connection.data(), 5, 0) == kDpErrInvalidParams);
    std::vector<std::uint8_t> ipx = buildAddress(
        {{kAidServiceProvider, hex("20 F4 85 68 54 89 CF 11 96 0C 00 80 C7 53 4E 82")}});
    CHECK(m.dp->InitializeConnection(ipx.data(), ipx.size(), 0) == kDpErrUnavailable);
    REQUIRE(m.dp->InitializeConnection(connection.data(), connection.size(), 0) == kDpOk);
    CHECK(m.dp->InitializeConnection(connection.data(), connection.size(), 0) ==
          kDpErrAlreadyInitialized);
}

TEST_CASE("the methods that need a connection say so before InitializeConnection") {
    Mocked m;
    SessionDesc d;
    Caps caps;
    CHECK(m.dp->EnumSessions(d, 0, [](const SessionDesc *, std::uint32_t &, std::uint32_t) {
        return false;
    }, 0) == kDpErrUninitialized);
    CHECK(m.dp->Open(d, kOpenCreate) == kDpErrUninitialized);
    CHECK(m.dp->GetCaps(&caps, 0) == kDpErrUninitialized);
    Dpid id = 0;
    CHECK(m.dp->CreatePlayer(&id, Name{}, 0, {}, 0) == kDpErrNoConnection);
    CHECK(m.dp->GetSessionDesc(&d) == kDpErrNoConnection);
    CHECK(m.dp->Close() == kDpOk);
    CHECK(m.transport.sent.empty());
}

TEST_CASE("EnumSessions broadcasts a request and reports the host's reply, then times out") {
    Mocked m;
    REQUIRE(initialize(*m.dp) == kDpOk);
    m.transport.incoming.push_back({mockEnumReply(1), kHostEndpoint});
    std::vector<SessionDesc> sessions;
    std::vector<std::uint32_t> flagsSeen;
    std::uint32_t start = m.clock.now;
    REQUIRE(m.dp->EnumSessions(applicationFilter(), 0,
                               [&](const SessionDesc *s, std::uint32_t &, std::uint32_t flags) {
                                   flagsSeen.push_back(flags);
                                   if (s != nullptr) {
                                       sessions.push_back(*s);
                                   }
                                   return s != nullptr; // on: next session; at the end: stop
                               },
                               0) == kDpOk);
    // The default timeout is DPCAPS.dwTimeout, 1000 ms.
    CHECK(m.clock.now - start == 1000u);
    REQUIRE(!m.transport.sent.empty());
    CHECK(m.transport.sent[0].bytes == kEnumRequest1);
    CHECK(m.transport.sent[0].peer == platform::Endpoint{0xFFFFFFFF, 47700});
    // Sent again every 250 ms while searching.
    CHECK(m.transport.sent.size() == 5u);
    for (const auto &d : m.transport.sent) {
        CHECK(d.bytes == kEnumRequest1);
    }
    REQUIRE(sessions.size() == 1u);
    CHECK(flagsSeen == std::vector<std::uint32_t>{0, kEscTimedOut});
    const SessionDesc &s = sessions[0];
    CHECK(s.flags == 0x2044u);
    CHECK(s.instance == mockInstance());
    CHECK(s.application == testApplication());
    CHECK(s.maxPlayers == 8u);
    CHECK(s.currentPlayers == 1u);
    CHECK(s.user == std::array<std::uint32_t, 4>{1, 2, 3, 4});
    CHECK(s.name == std::optional<std::string>("TestGame"));
    CHECK_FALSE(s.password);
}

TEST_CASE("EnumSessions searches again for as long as the callback asks") {
    Mocked m;
    REQUIRE(initialize(*m.dp) == kDpOk);
    int timeouts = 0;
    REQUIRE(m.dp->EnumSessions(applicationFilter(), 100,
                               [&](const SessionDesc *s, std::uint32_t &timeout, std::uint32_t) {
                                   if (s == nullptr && ++timeouts < 3) {
                                       timeout = 50;
                                       return true;
                                   }
                                   return false;
                               },
                               0) == kDpOk);
    CHECK(timeouts == 3);
    // Three searches, each a new request id.
    CHECK(m.transport.sent.front().bytes[8] == 1);
    CHECK(m.transport.sent.back().bytes[8] == 3);
}

TEST_CASE("EnumSessions ignores another application's session, a stale reply and a full one") {
    Mocked m;
    REQUIRE(initialize(*m.dp) == kDpOk);
    std::vector<std::uint8_t> other = mockEnumReply(1);
    other[32] ^= 0xFF; // the application GUID
    auto full = [](std::uint8_t request) {
        std::vector<std::uint8_t> reply = mockEnumReply(request);
        reply[52] = 8; // eight players of eight
        return reply;
    };
    m.transport.incoming.push_back({other, kHostEndpoint});
    m.transport.incoming.push_back({mockEnumReply(9), kHostEndpoint});
    m.transport.incoming.push_back({full(1), kHostEndpoint});
    int found = 0;
    auto count = [&](const SessionDesc *s, std::uint32_t &, std::uint32_t) {
        found += s != nullptr;
        return false;
    };
    REQUIRE(m.dp->EnumSessions(applicationFilter(), 50, count, 0) == kDpOk);
    CHECK(found == 0);
    // DPENUMSESSIONS_ALL also lists the full one; the second search is request 2.
    m.transport.incoming.push_back({full(2), kHostEndpoint});
    REQUIRE(m.dp->EnumSessions(applicationFilter(), 50, count, kEnumSessionsAll) == kDpOk);
    CHECK(found == 1);
}

TEST_CASE("EnumSessions with an INet address asks that host only") {
    Mocked m;
    std::vector<std::uint8_t> address = tcpIpAddress(0xC000020A, 2300);
    REQUIRE(m.dp->InitializeConnection(address.data(), address.size(), 0) == kDpOk);
    REQUIRE(m.dp->EnumSessions(applicationFilter(), 10,
                               [](const SessionDesc *, std::uint32_t &, std::uint32_t) {
                                   return false;
                               },
                               0) == kDpOk);
    REQUIRE(!m.transport.sent.empty());
    for (const auto &d : m.transport.sent) {
        CHECK(d.peer == kHostEndpoint);
    }
}

TEST_CASE("EnumSessions also asks this machine when loopback search is on") {
    Config config;
    config.searchLoopback = true;
    Mocked m(config);
    REQUIRE(initialize(*m.dp) == kDpOk);
    REQUIRE(m.dp->EnumSessions(applicationFilter(), 10,
                               [](const SessionDesc *, std::uint32_t &, std::uint32_t) {
                                   return false;
                               },
                               0) == kDpOk);
    REQUIRE(m.transport.sent.size() >= 2u);
    CHECK(m.transport.sent[0].peer == platform::Endpoint{0xFFFFFFFF, 47700});
    CHECK(m.transport.sent[1].peer == platform::Endpoint{0x7F000001, 47700});
}

TEST_CASE("asynchronous EnumSessions reports what has come in and keeps searching") {
    Mocked m;
    REQUIRE(initialize(*m.dp) == kDpOk);
    std::vector<SessionDesc> sessions;
    auto collect = [&](const SessionDesc *s, std::uint32_t &, std::uint32_t) {
        if (s != nullptr) {
            sessions.push_back(*s);
        }
        return true;
    };
    REQUIRE(m.dp->EnumSessions(applicationFilter(), 0, collect, kEnumSessionsAsync) == kDpOk);
    CHECK(sessions.empty());
    CHECK(m.transport.sent.size() == 1u);
    m.transport.incoming.push_back({mockEnumReply(1), kHostEndpoint});
    REQUIRE(m.dp->EnumSessions(applicationFilter(), 0, collect, kEnumSessionsAsync) == kDpOk);
    CHECK(sessions.size() == 1u);
    CHECK(m.transport.sent.size() == 1u); // the same search goes on
    REQUIRE(m.dp->EnumSessions(applicationFilter(), 0, nullptr, kEnumSessionsStopAsync) == kDpOk);
    m.clock.Advance(1000);
    m.dp->Service(0);
    CHECK(m.transport.sent.size() == 1u);
}

TEST_CASE("Open(DPOPEN_JOIN) asks the host and acknowledges its accept") {
    Mocked m;
    REQUIRE(initialize(*m.dp) == kDpOk);
    SessionDesc d;
    d.instance = mockInstance();
    CHECK(m.dp->Open(d, kOpenJoin) == kDpErrNoSessions); // not enumerated
    m.transport.incoming.push_back({mockEnumReply(1), kHostEndpoint});
    REQUIRE(m.dp->EnumSessions(applicationFilter(), 50,
                               [](const SessionDesc *, std::uint32_t &, std::uint32_t) {
                                   return false;
                               },
                               0) == kDpOk);
    m.transport.sent.clear();
    m.transport.incoming.push_back({mockJoinAccept(), kHostEndpoint});
    d.password = "pw";
    REQUIRE(m.dp->Open(d, kOpenJoin) == kDpOk);
    REQUIRE(m.transport.sent.size() == 2u);
    CHECK(m.transport.sent[0].bytes == joinRequest("02 00 70 77"));
    CHECK(m.transport.sent[0].peer == kHostEndpoint);
    CHECK(m.transport.sent[1].bytes ==
          hex("53 46 44 50 01 06 00 00 0D F0 AD 0B 02 00 00 00 01 00 00 00"));
    CHECK(m.transport.sent[1].peer == kHostEndpoint);

    SessionDesc now;
    REQUIRE(m.dp->GetSessionDesc(&now) == kDpOk);
    CHECK(now.instance == mockInstance());
    CHECK(now.name == std::optional<std::string>("TestGame"));
    CHECK(now.currentPlayers == 1u);
    CHECK(m.dp->Open(d, kOpenJoin) == kDpErrAlreadyInitialized);
}

TEST_CASE("Open(DPOPEN_JOIN) gives the host's refusal") {
    Mocked m;
    REQUIRE(initialize(*m.dp) == kDpOk);
    m.transport.incoming.push_back({mockEnumReply(1), kHostEndpoint});
    REQUIRE(m.dp->EnumSessions(applicationFilter(), 50,
                               [](const SessionDesc *, std::uint32_t &, std::uint32_t) {
                                   return false;
                               },
                               0) == kDpOk);
    // A refusal from another machine is not the host's.
    m.transport.incoming.push_back(
        {concat({hex("53 46 44 50 01 04 00 00"), guidBytes(mockInstance()), hex("4A 01 77 88")}),
         platform::Endpoint{0xC000020B, 2300}});
    m.transport.incoming.push_back(
        {concat({hex("53 46 44 50 01 04 00 00"), guidBytes(mockInstance()), hex("54 01 77 88")}),
         kHostEndpoint});
    SessionDesc d;
    d.instance = mockInstance();
    CHECK(m.dp->Open(d, kOpenJoin) == kDpErrInvalidPassword);
    // Not in a session, and free to try again.
    Dpid id = 0;
    CHECK(m.dp->CreatePlayer(&id, Name{}, 0, {}, 0) == kDpErrNoConnection);
}

TEST_CASE("Open(DPOPEN_JOIN) times out when the host does not answer, asking again meanwhile") {
    Mocked m;
    REQUIRE(initialize(*m.dp) == kDpOk);
    m.transport.incoming.push_back({mockEnumReply(1), kHostEndpoint});
    REQUIRE(m.dp->EnumSessions(applicationFilter(), 50,
                               [](const SessionDesc *, std::uint32_t &, std::uint32_t) {
                                   return false;
                               },
                               0) == kDpOk);
    m.transport.sent.clear();
    std::uint32_t start = m.clock.now;
    SessionDesc d;
    d.instance = mockInstance();
    CHECK(m.dp->Open(d, kOpenJoin) == kDpErrTimeout);
    CHECK(m.clock.now - start == 5000u);
    REQUIRE(m.transport.sent.size() >= 20u);
    for (const auto &s : m.transport.sent) {
        CHECK(s.bytes == joinRequest("FF FF"));
    }
}

TEST_CASE("Open(DPOPEN_CREATE) binds the enumeration port and answers enumeration") {
    Mocked m;
    REQUIRE(host(*m.dp, "TestGame", kSessionKeepAlive | kSessionMigrateHost, 8) == kDpOk);
    const std::vector<std::string> &calls = m.transport.log.Calls();
    CHECK(std::find(calls.begin(), calls.end(), "Bind(1, 0, 47700)") != calls.end());
    SessionDesc desc;
    REQUIRE(m.dp->GetSessionDesc(&desc) == kDpOk);
    CHECK_FALSE(desc.instance.IsNull());
    CHECK(desc.application == testApplication());
    CHECK(desc.currentPlayers == 0u);

    platform::Endpoint asker{0xC0000214, 50001};
    m.transport.incoming.push_back(
        {concat({hex("53 46 44 50 01 01 00 00 2A 00 00 00"), guidBytes(testApplication())}),
         asker});
    // A request for another application, and one for any application.
    m.transport.incoming.push_back(
        {concat({hex("53 46 44 50 01 01 00 00 2B 00 00 00"), guidBytes(mockInstance())}), asker});
    m.transport.incoming.push_back(
        {concat({hex("53 46 44 50 01 01 00 00 2C 00 00 00"), guidBytes(Guid{})}), asker});
    m.dp->Service(0);
    REQUIRE(m.transport.sent.size() == 2u);
    std::vector<std::uint8_t> body = concat(
        {hex("44 00 00 00"), guidBytes(desc.instance), guidBytes(testApplication()),
         hex("08 00 00 00 00 00 00 00"), hex("00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"),
         hex("08 00 54 65 73 74 47 61 6D 65 FF FF")});
    CHECK(m.transport.sent[0].bytes == concat({hex("53 46 44 50 01 02 00 00 2A 00 00 00"), body}));
    CHECK(m.transport.sent[0].peer == asker);
    CHECK(m.transport.sent[1].bytes == concat({hex("53 46 44 50 01 02 00 00 2C 00 00 00"), body}));
}

TEST_CASE("Open(DPOPEN_CREATE) fails when the enumeration port is taken") {
    Mocked m;
    REQUIRE(initialize(*m.dp) == kDpOk);
    m.transport.bindResult = platform::Error::kAddressInUse;
    SessionDesc d;
    d.application = testApplication();
    CHECK(m.dp->Open(d, kOpenCreate) == kDpErrCantCreateSession);
    m.transport.bindResult = {};
    CHECK(m.dp->Open(d, kOpenCreate | kOpenJoin) == kDpErrInvalidFlags);
}

TEST_CASE("a host admits a joiner: join accept on the channel, sequence 1") {
    Mocked m;
    REQUIRE(host(*m.dp, "TestGame") == kDpOk);
    SessionDesc desc;
    REQUIRE(m.dp->GetSessionDesc(&desc) == kDpOk);
    platform::Endpoint joiner{0xC0000214, 50001};
    std::vector<std::uint8_t> request =
        concat({hex("53 46 44 50 01 03 00 00"), guidBytes(desc.instance), hex("FF FF")});
    m.transport.incoming.push_back({request, joiner});
    m.transport.incoming.push_back({request, joiner}); // asked twice: admitted once
    m.dp->Service(0);
    REQUIRE(m.transport.sent.size() == 1u);
    const std::vector<std::uint8_t> accept = m.transport.sent[0].bytes;
    CHECK(m.transport.sent[0].peer == joiner);
    REQUIRE(accept.size() > 29u);
    CHECK(std::vector<std::uint8_t>(accept.begin(), accept.begin() + 8) ==
          hex("53 46 44 50 01 05 00 00"));
    CHECK(std::vector<std::uint8_t>(accept.begin() + 12, accept.begin() + 29) ==
          hex("01 00 00 00 01 00 00 00 01 02 00 00 00 03 00 00 00"));
    std::optional<wire::Packet> p = wire::decodePacket(accept.data(), accept.size());
    REQUIRE(p);
    std::optional<wire::Inner> inner = wire::decodeInner(p->body);
    REQUIRE(inner);
    CHECK(inner->desc.instance == desc.instance);
    CHECK(inner->peers.empty());
    CHECK(inner->entities.empty());

    // Unacknowledged, it is sent again after 200 ms; acknowledged, it is not.
    m.clock.Advance(200);
    m.dp->Service(0);
    REQUIRE(m.transport.sent.size() == 2u);
    CHECK(m.transport.sent[1].bytes == accept);
    std::vector<std::uint8_t> key(accept.begin() + 8, accept.begin() + 12);
    m.transport.incoming.push_back(
        {concat({hex("53 46 44 50 01 06 00 00"), key, hex("02 00 00 00 01 00 00 00")}), joiner});
    m.dp->Service(0);
    m.clock.Advance(200);
    m.dp->Service(0);
    CHECK(m.transport.sent.size() == 2u);
}

TEST_CASE("a host refuses a wrong password, a full session and a closed one") {
    Mocked m;
    REQUIRE(host(*m.dp, "TestGame", kSessionKeepAlive, 1, std::string("pw")) == kDpOk);
    SessionDesc desc;
    REQUIRE(m.dp->GetSessionDesc(&desc) == kDpOk);
    platform::Endpoint joiner{0xC0000214, 50001};
    std::vector<std::uint8_t> instance = guidBytes(desc.instance);
    auto ask = [&](const char *password) {
        m.transport.sent.clear();
        m.transport.incoming.push_back(
            {concat({hex("53 46 44 50 01 03 00 00"), instance, hex(password)}), joiner});
        m.dp->Service(0);
        REQUIRE(m.transport.sent.size() == 1u);
        return m.transport.sent[0].bytes;
    };
    std::vector<std::uint8_t> reject = concat({hex("53 46 44 50 01 04 00 00"), instance});
    CHECK(ask("FF FF") == concat({reject, hex("54 01 77 88")}));
    CHECK(ask("02 00 70 78") == concat({reject, hex("54 01 77 88")}));

    Dpid captain = 0;
    REQUIRE(m.dp->CreatePlayer(&captain, Name{}, 0, {}, 0) == kDpOk);
    CHECK(ask("02 00 70 77") == concat({reject, hex("4A 01 77 88")})); // one player of one

    SessionDesc open = desc;
    open.maxPlayers = 0;
    open.flags |= kSessionJoinDisabled;
    REQUIRE(m.dp->SetSessionDesc(open, 0) == kDpOk);
    CHECK(ask("02 00 70 77") == concat({reject, hex("4A 01 77 88")}));
}

TEST_CASE("only the host may change the session description") {
    MockJoined j;
    SessionDesc d;
    REQUIRE(j.dp->GetSessionDesc(&d) == kDpOk);
    CHECK(j.dp->SetSessionDesc(d, 0) == kDpErrAccessDenied);
}

TEST_CASE("GetCaps") {
    Mocked m;
    REQUIRE(initialize(*m.dp) == kDpOk);
    Caps caps;
    CHECK(m.dp->GetCaps(nullptr, 0) == kDpErrInvalidParams);
    REQUIRE(m.dp->GetCaps(&caps, 0) == kDpOk);
    CHECK(caps.flags == 0x12070u);
    CHECK(caps.maxBufferSize == 60000u);
    CHECK(caps.timeout == 1000u);
    REQUIRE(host(*m.dp, "TestGame") == kDpErrAlreadyInitialized);
    SessionDesc d;
    d.application = testApplication();
    REQUIRE(m.dp->Open(d, kOpenCreate) == kDpOk);
    REQUIRE(m.dp->GetCaps(&caps, kGetCapsGuaranteed) == kDpOk);
    CHECK(caps.flags == 0x12072u); // DPCAPS_ISHOST
}

TEST_CASE("Close tells the host, twice, and leaves the object ready to join again") {
    MockJoined j;
    REQUIRE(j.dp->Close() == kDpOk);
    std::vector<std::uint8_t> leave = hex("53 46 44 50 01 09 00 00 0D F0 AD 0B 02 00 00 00");
    REQUIRE(j.transport.sent.size() == 2u);
    CHECK(j.transport.sent[0].bytes == leave);
    CHECK(j.transport.sent[1].bytes == leave);
    CHECK(j.transport.sent[0].peer == kHostEndpoint);
    SessionDesc d;
    CHECK(j.dp->GetSessionDesc(&d) == kDpErrNoConnection);
    // The enumerated session is still known.
    j.transport.incoming.push_back({mockJoinAccept(), kHostEndpoint});
    d.instance = mockInstance();
    CHECK(j.dp->Open(d, kOpenJoin) == kDpOk);
}

TEST_CASE("datagrams from strangers or with the wrong key are ignored") {
    MockJoined j;
    Dpid self = 0;
    REQUIRE(j.dp->CreatePlayer(&self, Name{}, 0, {}, 0) == kDpOk);
    drain(*j.dp);
    std::vector<std::uint8_t> data = hex("53 46 44 50 01 07 00 00 0D F0 AD 0B 01 00 00 00"
                                         "01 00 01 00 01 00 02 00 AA");
    std::vector<std::uint8_t> wrongKey = data;
    wrongKey[8] = 0x0E;
    std::vector<std::uint8_t> stranger = data;
    stranger[12] = 7; // peer 7 is not in the session
    j.transport.incoming.push_back({wrongKey, kHostEndpoint});
    j.transport.incoming.push_back({stranger, kHostEndpoint});
    j.transport.incoming.push_back({hex("00 01 02 03"), kHostEndpoint});
    CHECK(drain(*j.dp).empty());
    j.transport.incoming.push_back({data, kHostEndpoint});
    CHECK(drain(*j.dp).size() == 1u);
}

} // namespace sfcop::hle::dplay
