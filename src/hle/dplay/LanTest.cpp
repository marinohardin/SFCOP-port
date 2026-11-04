// Several DirectPlay objects on one in-memory network (DplayTestUtil.h): what one machine does,
// the others see.
#include "hle/dplay/DirectPlay.h"

#include "hle/dplay/DplayMockFixture.h"
#include "hle/dplay/DplayTestUtil.h"

#include <doctest/doctest.h>

namespace sfcop::hle::dplay {

using namespace test;

namespace {

Name shortName(const char *text) {
    Name n;
    n.shortName = text;
    return n;
}

std::vector<std::uint32_t> types(const std::vector<ReceivedMessage> &messages) {
    std::vector<std::uint32_t> out;
    for (const ReceivedMessage &m : messages) {
        out.push_back(m.from == kDpidSysMsg ? typeOf(m) : 0xFFFFFFFF);
    }
    return out;
}

} // namespace

TEST_CASE("three machines: host, two joiners, every player seen everywhere") {
    Lan lan(3);
    REQUIRE(host(lan[0], "TestGame") == kDpOk);
    Dpid captain = 0;
    REQUIRE(lan[0].CreatePlayer(&captain, shortName("TestCaptain"), 0, {}, 0) == kDpOk);
    REQUIRE(join(lan[1]) == kDpOk);
    Dpid helm = 0;
    REQUIRE(lan[1].CreatePlayer(&helm, shortName("Helm"), 0, {}, 0) == kDpOk);
    REQUIRE(join(lan[2]) == kDpOk);
    Dpid gunner = 0;
    REQUIRE(lan[2].CreatePlayer(&gunner, shortName("Gunner"), 0, {}, 0) == kDpOk);
    lan.Run(100);
    CHECK(captain == 0x00010001u);
    CHECK(helm == 0x00020001u);
    CHECK(gunner == 0x00030001u);

    for (std::size_t i = 0; i < 3; ++i) {
        CAPTURE(i);
        std::vector<Dpid> ids;
        REQUIRE(lan[i].EnumPlayers(nullptr, [&](const PlayerEntry &p) {
            ids.push_back(p.id);
            return true;
        }, 0) == kDpOk);
        CHECK(ids == std::vector<Dpid>{captain, helm, gunner});
        SessionDesc d;
        REQUIRE(lan[i].GetSessionDesc(&d) == kDpOk);
        CHECK(d.currentPlayers == 3u);
        CHECK(d.name == std::optional<std::string>("TestGame"));
    }
    CHECK(types(drain(lan[0])) == std::vector<std::uint32_t>{kSysCreatePlayerOrGroup,
                                                             kSysCreatePlayerOrGroup});
    CHECK(types(drain(lan[1])) == std::vector<std::uint32_t>{kSysCreatePlayerOrGroup});
    CHECK(drain(lan[2]).empty());

    // The joiners talk to each other directly.
    std::vector<std::uint8_t> payload = hex("01 00 00 00 02 00 00 00 01 00");
    REQUIRE(lan[2].Send(gunner, helm, kSendGuaranteed, payload.data(), payload.size()) == kDpOk);
    lan.Run(50);
    std::vector<ReceivedMessage> got = drain(lan[1]);
    REQUIRE(got.size() == 1u);
    CHECK(got[0].from == gunner);
    CHECK(got[0].blob.bytes == payload);
    CHECK(drain(lan[0]).empty());
}

TEST_CASE("a joiner's enumeration finds the session with its current player count") {
    Lan lan(2);
    REQUIRE(host(lan[0], "TestGame", kSessionKeepAlive, 4) == kDpOk);
    Dpid captain = 0;
    REQUIRE(lan[0].CreatePlayer(&captain, Name{}, 0, {}, 0) == kDpOk);
    REQUIRE(initialize(lan[1]) == kDpOk);
    SessionDesc filter;
    filter.application = testApplication();
    std::vector<SessionDesc> found;
    REQUIRE(lan[1].EnumSessions(filter, 100, [&](const SessionDesc *s, std::uint32_t &,
                                                 std::uint32_t) {
        if (s != nullptr) {
            found.push_back(*s);
        }
        return s != nullptr;
    }, 0) == kDpOk);
    REQUIRE(found.size() == 1u);
    CHECK(found[0].maxPlayers == 4u);
    CHECK(found[0].currentPlayers == 1u);
    SessionDesc hosted;
    REQUIRE(lan[0].GetSessionDesc(&hosted) == kDpOk);
    CHECK(found[0].instance == hosted.instance);
}

TEST_CASE("a password session: the wrong password is refused, the right one admitted") {
    Lan lan(2);
    REQUIRE(host(lan[0], "TestGame", kSessionKeepAlive, 0, std::string("pw")) == kDpOk);
    CHECK(join(lan[1]) == kDpErrInvalidPassword);
    CHECK(join(lan[1], std::string("px")) == kDpErrAlreadyInitialized);
    SessionDesc d;
    // Already initialized: join() would fail; enumerate and open by hand.
    SessionDesc filter;
    std::optional<Guid> instance;
    REQUIRE(lan[1].EnumSessions(filter, 100, [&](const SessionDesc *s, std::uint32_t &,
                                                 std::uint32_t) {
        if (s != nullptr) {
            instance = s->instance;
            CHECK((s->flags & kSessionPasswordRequired) != 0u);
            CHECK_FALSE(s->password);
        }
        return false;
    }, 0) == kDpOk);
    REQUIRE(instance);
    d.instance = *instance;
    d.password = "px";
    CHECK(lan[1].Open(d, kOpenJoin) == kDpErrInvalidPassword);
    d.password = "pw";
    CHECK(lan[1].Open(d, kOpenJoin) == kDpOk);
}

TEST_CASE("the session description changes on every machine") {
    Lan lan(2);
    REQUIRE(host(lan[0], "TestGame") == kDpOk);
    REQUIRE(join(lan[1]) == kDpOk);
    Dpid helm = 0;
    REQUIRE(lan[1].CreatePlayer(&helm, Name{}, 0, {}, 0) == kDpOk);
    lan.Run(50);
    SessionDesc d;
    REQUIRE(lan[0].GetSessionDesc(&d) == kDpOk);
    d.name = "Renamed";
    d.user = {9, 8, 7, 6};
    REQUIRE(lan[0].SetSessionDesc(d, 0) == kDpOk);
    lan.Run(50);
    SessionDesc seen;
    REQUIRE(lan[1].GetSessionDesc(&seen) == kDpOk);
    CHECK(seen.name == std::optional<std::string>("Renamed"));
    CHECK(seen.user == std::array<std::uint32_t, 4>{9, 8, 7, 6});
    CHECK(seen.instance == d.instance);
    CHECK(types(drain(lan[1])) == std::vector<std::uint32_t>{kSysSetSessionDesc});
}

TEST_CASE("guaranteed messages arrive once and in order across a lossy network") {
    Lan lan(2);
    REQUIRE(host(lan[0], "TestGame") == kDpOk);
    REQUIRE(join(lan[1]) == kDpOk);
    Dpid captain = 0, helm = 0;
    REQUIRE(lan[0].CreatePlayer(&captain, Name{}, 0, {}, 0) == kDpOk);
    REQUIRE(lan[1].CreatePlayer(&helm, Name{}, 0, {}, 0) == kDpOk);
    lan.Run(100);
    drain(lan[0]);
    drain(lan[1]);
    std::size_t n = 0;
    lan.net.drop = [&](platform::Endpoint, platform::Endpoint, const std::vector<std::uint8_t> &) {
        return ++n % 3 == 0; // every third datagram, either way
    };
    for (std::uint8_t i = 0; i < 30; ++i) {
        std::vector<std::uint8_t> payload = {1, 0, 0, 0, 1, 0, 0, 0, i};
        REQUIRE(lan[0].Send(captain, helm, kSendGuaranteed, payload.data(), payload.size()) ==
                kDpOk);
    }
    lan.Run(3000);
    std::vector<ReceivedMessage> got = drain(lan[1]);
    REQUIRE(got.size() == 30u);
    for (std::uint8_t i = 0; i < 30; ++i) {
        CHECK(got[i].blob.bytes.back() == i);
    }
    std::uint32_t waiting = 99;
    REQUIRE(lan[0].GetMessageQueue(0, 0, kMessageQueueSend, &waiting, nullptr) == kDpOk);
    CHECK(waiting == 0u);
}

TEST_CASE("the host leaves: the lowest remaining peer hosts and the session goes on") {
    Lan lan(4); // the fourth machine comes later
    REQUIRE(host(lan[0], "TestGame") == kDpOk);
    REQUIRE(join(lan[1]) == kDpOk);
    REQUIRE(join(lan[2]) == kDpOk);
    Dpid captain = 0, helm = 0, gunner = 0;
    REQUIRE(lan[0].CreatePlayer(&captain, Name{}, 0, {}, 0) == kDpOk);
    REQUIRE(lan[1].CreatePlayer(&helm, Name{}, 0, {}, 0) == kDpOk);
    REQUIRE(lan[2].CreatePlayer(&gunner, Name{}, 0, {}, 0) == kDpOk);
    lan.Run(100);
    drain(lan[1]);
    drain(lan[2]);

    REQUIRE(lan[0].Close() == kDpOk);
    lan.Run(50);
    CHECK(types(drain(lan[1])) ==
          std::vector<std::uint32_t>{kSysDestroyPlayerOrGroup, kSysHost});
    CHECK(types(drain(lan[2])) == std::vector<std::uint32_t>{kSysDestroyPlayerOrGroup});
    Caps caps;
    REQUIRE(lan[1].GetCaps(&caps, 0) == kDpOk);
    CHECK((caps.flags & kCapsIsHost) != 0u);
    REQUIRE(lan[2].GetCaps(&caps, 0) == kDpOk);
    CHECK((caps.flags & kCapsIsHost) == 0u);

    // The two that are left still talk.
    std::vector<std::uint8_t> payload =
        hex("02 00 00 00 0C 00 00 00 00 00 00 00 00 00 00 00 01 00 00 00");
    REQUIRE(lan[2].Send(gunner, helm, 0, payload.data(), payload.size()) == kDpOk);
    lan.Run(20);
    std::vector<ReceivedMessage> got = drain(lan[1]);
    REQUIRE(got.size() == 1u);
    CHECK(got[0].blob.bytes == payload);

    // The new host answers enumeration and admits a new machine, as peer 4.
    REQUIRE(join(lan[3]) == kDpOk);
    Dpid pilot = 0;
    REQUIRE(lan[3].CreatePlayer(&pilot, Name{}, 0, {}, 0) == kDpOk);
    CHECK(pilot == 0x00040001u);
    lan.Run(100);
    CHECK(types(drain(lan[1])) == std::vector<std::uint32_t>{kSysCreatePlayerOrGroup});
    CHECK(types(drain(lan[2])) == std::vector<std::uint32_t>{kSysCreatePlayerOrGroup});
}

TEST_CASE("a machine that falls silent is dropped by the others") {
    Lan lan(3);
    REQUIRE(host(lan[0], "TestGame") == kDpOk);
    REQUIRE(join(lan[1]) == kDpOk);
    REQUIRE(join(lan[2]) == kDpOk);
    Dpid gunner = 0, captain = 0;
    REQUIRE(lan[0].CreatePlayer(&captain, Name{}, 0, {}, 0) == kDpOk);
    REQUIRE(lan[2].CreatePlayer(&gunner, Name{}, 0, {}, 0) == kDpOk);
    lan.Run(100);
    drain(lan[0]);
    std::uint32_t silent = lan.nodes[2]->transport->Address();
    lan.net.drop = [&](platform::Endpoint from, platform::Endpoint,
                       const std::vector<std::uint8_t> &) { return from.address == silent; };
    lan.Run(10500);
    std::vector<ReceivedMessage> got = drain(lan[0]);
    REQUIRE(got.size() == 1u);
    CHECK(typeOf(got[0]) == kSysDestroyPlayerOrGroup);
    Name n;
    CHECK(lan[0].GetPlayerName(gunner, &n) == kDpErrInvalidPlayer);
    CHECK(lan[1].GetPlayerName(gunner, &n) == kDpErrInvalidPlayer);
}

TEST_CASE("without DPSESSION_MIGRATEHOST the host's leaving loses the session") {
    Lan lan(2);
    REQUIRE(host(lan[0], "TestGame", kSessionKeepAlive) == kDpOk);
    REQUIRE(join(lan[1]) == kDpOk);
    Dpid helm = 0;
    REQUIRE(lan[1].CreatePlayer(&helm, Name{}, 0, {}, 0) == kDpOk);
    lan.Run(50);
    REQUIRE(lan[0].Close() == kDpOk);
    lan.Run(50);
    CHECK(types(drain(lan[1])) == std::vector<std::uint32_t>{kSysSessionLost});
    Caps caps;
    REQUIRE(lan[1].GetCaps(&caps, 0) == kDpOk);
    CHECK((caps.flags & kCapsIsHost) == 0u);

    // The session is gone: what needs it answers DPERR_SESSIONLOST, until the game closes it
    // and opens another.
    Dpid another = 0;
    std::vector<std::uint8_t> payload = {1, 0, 0, 0};
    SessionDesc desc;
    CHECK(lan[1].CreatePlayer(&another, Name{}, 0, {}, 0) == kDpErrSessionLost);
    CHECK(lan[1].Send(helm, 0, 0, payload.data(), payload.size()) == kDpErrSessionLost);
    CHECK(lan[1].GetSessionDesc(&desc) == kDpErrSessionLost);
    CHECK(lan[1].EnumPlayers(nullptr, [](const PlayerEntry &) { return true; }, 0) ==
          kDpErrSessionLost);
    CHECK(lan[1].Close() == kDpOk);
    CHECK(lan[1].GetSessionDesc(&desc) == kDpErrNoConnection);
    desc.application = testApplication();
    CHECK(lan[1].Open(desc, kOpenCreate) == kDpOk);
}

TEST_CASE("a local player's event is signalled for each message it gets") {
    Lan lan(2);
    REQUIRE(host(lan[0], "TestGame") == kDpOk);
    REQUIRE(join(lan[1]) == kDpOk);
    Dpid captain = 0, helm = 0;
    REQUIRE(lan[0].CreatePlayer(&captain, Name{}, 0, {}, 0) == kDpOk);
    REQUIRE(lan[1].CreatePlayer(&helm, Name{}, 0x1234, {}, 0) == kDpOk);
    lan.Run(50);
    lan.nodes[1]->signalled.clear();
    std::vector<std::uint8_t> payload = hex("01 00 00 00 00 00 00 00");
    REQUIRE(lan[0].Send(captain, helm, kSendGuaranteed, payload.data(), payload.size()) == kDpOk);
    REQUIRE(lan[0].Send(captain, helm, 0, payload.data(), payload.size()) == kDpOk);
    lan.Run(20);
    CHECK(lan.nodes[1]->signalled == std::vector<std::uint32_t>{0x1234, 0x1234});
    CHECK(lan.nodes[0]->signalled.empty());
}

namespace {

// A channel message from peer 3 of the mock host's session: sequence `seq` and an inner message.
std::vector<std::uint8_t> fromPeer3(std::uint8_t seq, const std::string &inner) {
    return concat({hex("53 46 44 50 01 05 00 00 0D F0 AD 0B 03 00 00 00"), {seq, 0, 0, 0},
                   hex(inner)});
}

const platform::Endpoint kPeer3{0xC000020B, 2301};

} // namespace

TEST_CASE("a peer cannot change another peer's player") {
    MockJoined j;
    Dpid helm = 0;
    REQUIRE(j.dp->CreatePlayer(&helm, Name{}, 0, {}, 0) == kDpOk);
    // The host tells of peer 3 (192.0.2.11:2301) and makes the group 0x00010002.
    j.transport.incoming.push_back(
        {mockReliable(2, hex("02 03 00 00 00 0B 02 00 C0 FD 08")), kHostEndpoint});
    j.transport.incoming.push_back(
        {mockReliable(3, hex("04 02 00 01 00 01 00 00 00 00 01 00 00 00 FF FF FF FF"
                             "00 00 00 00 00 00")),
         kHostEndpoint});
    REQUIRE(drain(*j.dp).size() == 1u); // the group
    CHECK(j.dp->GetCounters().rejectedChanges == 0u);

    // Peer 3 changes the host's player: data, name, removal; changes the host's group's
    // membership; announces a player as the host's; announces one with an id from the host's
    // range; and announces its own, which is allowed.
    const char *own = "FF FF FF FF 00 00 00 00 00 00"; // no name, no data, no members
    const std::vector<std::string> frames = {
        "06 01 00 01 00 02 00 00 00 11 22",
        "07 01 00 01 00 03 00 4E 65 77 FF FF",
        "05 01 00 01 00",
        "08 02 00 01 00 01 00 01 00",
        std::string("04 09 00 01 00 00 00 00 00 00 01 00 00 00 ") + own,
        std::string("04 09 00 01 00 00 00 00 00 00 03 00 00 00 ") + own,
    };
    std::uint8_t seq = 1;
    for (const std::string &inner : frames) {
        j.transport.incoming.push_back({fromPeer3(seq++, inner), kPeer3});
    }
    CHECK(drain(*j.dp).empty()); // nothing reached the local player
    CHECK(j.dp->GetCounters().rejectedChanges == 6u);

    std::vector<std::uint8_t> data;
    REQUIRE(j.dp->GetPlayerData(0x00010001, &data, 0) == kDpOk);
    CHECK(data == hex("DE AD BE EF"));
    Name name;
    REQUIRE(j.dp->GetPlayerName(0x00010001, &name) == kDpOk);
    CHECK(name.shortName == std::optional<std::string>("TestCaptain"));
    std::vector<Dpid> listed;
    REQUIRE(j.dp->EnumPlayers(nullptr, [&](const PlayerEntry &p) {
        listed.push_back(p.id);
        return true;
    }, kEnumPlayersGroup) == kDpOk);
    CHECK(listed == std::vector<Dpid>{0x00010001, 0x00010002, 0x00020001});

    // Its own player is taken, and it may change and remove it.
    j.transport.incoming.push_back(
        {fromPeer3(seq++, std::string("04 01 00 03 00 00 00 00 00 00 03 00 00 00 ") + own),
         kPeer3});
    REQUIRE(drain(*j.dp).size() == 1u);
    j.transport.incoming.push_back({fromPeer3(seq++, "06 01 00 03 00 02 00 00 00 11 22"), kPeer3});
    REQUIRE(j.dp->GetPlayerData(0x00030001, &data, 0) == kDpOk);
    CHECK(data == hex("11 22"));
    j.transport.incoming.push_back({fromPeer3(seq++, "05 01 00 03 00"), kPeer3});
    REQUIRE(drain(*j.dp).size() == 2u); // its data change, then its removal
    CHECK(j.dp->GetPlayerData(0x00030001, &data, 0) == kDpErrInvalidPlayer);
    CHECK(j.dp->GetCounters().rejectedChanges == 6u);

    // The same rule for this machine's own calls: a group of the host's with its player.
    CHECK(j.dp->AddPlayerToGroup(0x00010002, 0x00010001) == kDpErrAccessDenied);
    CHECK(j.dp->AddPlayerToGroup(0x00010002, helm) == kDpOk);
}

} // namespace sfcop::hle::dplay
