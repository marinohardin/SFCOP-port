// IDirectPlay4A players and groups on TransportMock, joined to the mock host's session as peer 2
// (DplayMockFixture.h): the host's player is 0x00010001 "TestCaptain".
#include "hle/dplay/DirectPlay.h"

#include "hle/dplay/DplayMockFixture.h"

#include <doctest/doctest.h>

namespace sfcop::hle::dplay {

using namespace test;

namespace {

constexpr Dpid kCaptain = 0x00010001;

// What the joiner sends the host on the channel: sequence `seq` and an inner message.
std::vector<std::uint8_t> toHost(std::uint8_t seq, const char *inner) {
    return concat({hex("53 46 44 50 01 05 00 00 0D F0 AD 0B 02 00 00 00"), {seq, 0, 0, 0},
                   hex(inner)});
}

std::vector<std::uint8_t> ack(std::uint8_t cumulative) {
    return concat({hex("53 46 44 50 01 06 00 00 0D F0 AD 0B 02 00 00 00"),
                   {cumulative, 0, 0, 0}});
}

Name shortName(const char *text) {
    Name n;
    n.shortName = text;
    return n;
}

// Joined, with a local player 0x00020001 "Helm" whose remote data is AA.
struct WithHelm : MockJoined {
    Dpid helm = 0;
    WithHelm() {
        REQUIRE(dp->CreatePlayer(&helm, shortName("Helm"), 0, hex("AA"), 0) == kDpOk);
        transport.sent.clear();
    }
};

// The host's player 0x00010002 "Gunner" arrives, channel sequence 2.
const std::vector<std::uint8_t> kGunnerAdd =
    mockReliable(2, hex("04 02 00 01 00 00 00 00 00 00 01 00 00 00"
                        "06 00 47 75 6E 6E 65 72 FF FF 00 00 00 00 00 00"));

} // namespace

TEST_CASE("CreatePlayer: the owner's peer number and a counter, announced to the host") {
    MockJoined j;
    Dpid helm = 0;
    REQUIRE(j.dp->CreatePlayer(&helm, shortName("Helm"), 0, hex("AA"), 0) == kDpOk);
    CHECK(helm == 0x00020001u);
    REQUIRE(j.transport.sent.size() == 1u);
    CHECK(j.transport.sent[0].bytes ==
          toHost(1, "04 01 00 02 00 00 00 00 00 00 02 00 00 00 04 00 48 65 6C 6D FF FF"
                    "01 00 00 00 AA 00 00"));
    CHECK(j.transport.sent[0].peer == kHostEndpoint);
    // A player's own creation is not reported to it.
    CHECK(drain(*j.dp).empty());
    Dpid second = 0;
    REQUIRE(j.dp->CreatePlayer(&second, Name{}, 0, {}, kPlayerSpectator) == kDpOk);
    CHECK(second == 0x00020002u);
    CHECK(j.dp->CreatePlayer(&second, Name{}, 0, {}, 0x1) == kDpErrInvalidFlags);
    CHECK(j.dp->CreatePlayer(nullptr, Name{}, 0, {}, 0) == kDpErrInvalidParams);
}

TEST_CASE("a remote player's creation reaches the local players as DPSYS_CREATEPLAYERORGROUP") {
    WithHelm j;
    j.transport.incoming.push_back({kGunnerAdd, kHostEndpoint});
    std::vector<ReceivedMessage> got = drain(*j.dp);
    REQUIRE(got.size() == 1u);
    CHECK(got[0].from == kDpidSysMsg);
    CHECK(got[0].to == j.helm);
    CHECK(got[0].blob.bytes == hex("03 00 00 00 01 00 00 00 02 00 01 00 03 00 00 00"
                                   "00 00 00 00 00 00 00 00 10 00 00 00 00 00 00 00"
                                   "30 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"
                                   "47 75 6E 6E 65 72 00"));
    CHECK(got[0].blob.pointers == std::vector<std::uint32_t>{32});
    REQUIRE(j.transport.sent.size() == 1u);
    CHECK(j.transport.sent[0].bytes == ack(2));
}

TEST_CASE("a remote player's destruction: DPSYS_DESTROYPLAYERORGROUP") {
    WithHelm j;
    j.transport.incoming.push_back({kGunnerAdd, kHostEndpoint});
    j.transport.incoming.push_back({mockReliable(3, hex("05 02 00 01 00")), kHostEndpoint});
    std::vector<ReceivedMessage> got = drain(*j.dp);
    REQUIRE(got.size() == 2u);
    CHECK(typeOf(got[0]) == kSysCreatePlayerOrGroup);
    CHECK(got[1].blob.bytes == hex("05 00 00 00 01 00 00 00 02 00 01 00 00 00 00 00"
                                   "00 00 00 00 00 00 00 00 00 00 00 00 10 00 00 00"
                                   "00 00 00 00 34 00 00 00 00 00 00 00 00 00 00 00"
                                   "00 00 00 00 47 75 6E 6E 65 72 00"));
    CHECK(got[1].blob.pointers == std::vector<std::uint32_t>{36});
    Name n;
    CHECK(j.dp->GetPlayerName(0x00010002, &n) == kDpErrInvalidPlayer);
}

TEST_CASE("channel messages are taken in order, once") {
    WithHelm j;
    j.transport.incoming.push_back({kGunnerAdd, kHostEndpoint});
    drain(*j.dp);
    j.transport.sent.clear();
    // Sequence 4 (a new name) arrives before 3 (new data): it waits.
    j.transport.incoming.push_back(
        {mockReliable(4, hex("07 01 00 01 00 03 00 4E 65 77 FF FF")), kHostEndpoint});
    CHECK(drain(*j.dp).empty());
    REQUIRE(j.transport.sent.size() == 1u);
    CHECK(j.transport.sent[0].bytes == ack(2));
    j.transport.incoming.push_back(
        {mockReliable(3, hex("06 01 00 01 00 02 00 00 00 11 22")), kHostEndpoint});
    std::vector<ReceivedMessage> got = drain(*j.dp);
    REQUIRE(got.size() == 2u);
    CHECK(got[0].blob.bytes ==
          hex("02 01 00 00 01 00 00 00 01 00 01 00 14 00 00 00 02 00 00 00 11 22"));
    CHECK(got[1].blob.bytes == hex("03 01 00 00 01 00 00 00 01 00 01 00 10 00 00 00 00 00 00 00"
                                   "1C 00 00 00 00 00 00 00 4E 65 77 00"));
    REQUIRE(j.transport.sent.size() == 2u);
    CHECK(j.transport.sent[1].bytes == ack(4));
    // Sequence 3 again: already taken, acknowledged again.
    j.transport.incoming.push_back(
        {mockReliable(3, hex("06 01 00 01 00 02 00 00 00 11 22")), kHostEndpoint});
    CHECK(drain(*j.dp).empty());
    CHECK(j.transport.sent.back().bytes == ack(4));
    std::vector<std::uint8_t> data;
    REQUIRE(j.dp->GetPlayerData(kCaptain, &data, 0) == kDpOk);
    CHECK(data == hex("11 22"));
    Name n;
    REQUIRE(j.dp->GetPlayerName(kCaptain, &n) == kDpOk);
    CHECK(n.shortName == std::optional<std::string>("New"));
}

TEST_CASE("EnumPlayers lists the name table with its local and remote flags") {
    WithHelm j;
    Dpid group = 0;
    REQUIRE(j.dp->CreateGroup(&group, shortName("Fleet"), {}, 0) == kDpOk);
    std::vector<PlayerEntry> seen;
    auto collect = [&](const PlayerEntry &p) {
        seen.push_back(p);
        return true;
    };
    REQUIRE(j.dp->EnumPlayers(nullptr, collect, 0) == kDpOk);
    REQUIRE(seen.size() == 2u);
    CHECK(seen[0].id == kCaptain);
    CHECK(seen[0].flags == kEnumPlayersRemote);
    CHECK(seen[0].playerType == kPlayerTypePlayer);
    CHECK(seen[0].name.shortName == std::optional<std::string>("TestCaptain"));
    CHECK(seen[1].id == j.helm);
    CHECK(seen[1].flags == kEnumPlayersLocal);
    seen.clear();
    REQUIRE(j.dp->EnumPlayers(nullptr, collect, kEnumPlayersLocal) == kDpOk);
    REQUIRE(seen.size() == 1u);
    CHECK(seen[0].id == j.helm);
    seen.clear();
    REQUIRE(j.dp->EnumPlayers(nullptr, collect, kEnumPlayersRemote) == kDpOk);
    REQUIRE(seen.size() == 1u);
    CHECK(seen[0].id == kCaptain);
    seen.clear();
    REQUIRE(j.dp->EnumPlayers(nullptr, collect, kEnumPlayersGroup) == kDpOk);
    CHECK(seen.size() == 3u);
    seen.clear();
    REQUIRE(j.dp->EnumPlayers(nullptr, [&](const PlayerEntry &p) {
        seen.push_back(p);
        return false;
    }, 0) == kDpOk);
    CHECK(seen.size() == 1u);
    seen.clear();
    REQUIRE(j.dp->EnumGroups(nullptr, collect, 0) == kDpOk);
    REQUIRE(seen.size() == 1u);
    CHECK(seen[0].id == group);
    CHECK(seen[0].playerType == kPlayerTypeGroup);
    CHECK(seen[0].flags == kEnumPlayersLocal);
    CHECK(j.dp->EnumPlayers(nullptr, collect, kEnumPlayersSession) == kDpErrUnsupported);
    CHECK(j.dp->EnumPlayers(nullptr, nullptr, 0) == kDpErrInvalidParams);
}

TEST_CASE("names: a local player's change goes to the host; a remote one's is refused") {
    WithHelm j;
    REQUIRE(j.dp->SetPlayerName(j.helm, shortName("XO"), 0) == kDpOk);
    REQUIRE(j.transport.sent.size() == 1u);
    CHECK(j.transport.sent[0].bytes == toHost(2, "07 01 00 02 00 02 00 58 4F FF FF"));
    Name n;
    REQUIRE(j.dp->GetPlayerName(j.helm, &n) == kDpOk);
    CHECK(n.shortName == std::optional<std::string>("XO"));
    CHECK(j.dp->SetPlayerName(kCaptain, shortName("XO"), 0) == kDpErrAccessDenied);
    CHECK(j.dp->GetPlayerName(0x00030001, &n) == kDpErrInvalidPlayer);
    CHECK(j.dp->GetPlayerName(j.helm, nullptr) == kDpErrInvalidParams);
}

TEST_CASE("player data: remote data travels, local data stays here") {
    WithHelm j;
    std::vector<std::uint8_t> data;
    REQUIRE(j.dp->GetPlayerData(kCaptain, &data, 0) == kDpOk);
    CHECK(data == hex("DE AD BE EF"));
    REQUIRE(j.dp->GetPlayerData(kCaptain, &data, kGetLocal) == kDpOk);
    CHECK(data.empty());

    REQUIRE(j.dp->SetPlayerData(j.helm, hex("01 02"), kSetGuaranteed) == kDpOk);
    REQUIRE(j.transport.sent.size() == 1u);
    CHECK(j.transport.sent[0].bytes == toHost(2, "06 01 00 02 00 02 00 00 00 01 02"));
    // Unguaranteed, it still goes on the channel.
    REQUIRE(j.dp->SetPlayerData(j.helm, hex("03"), 0) == kDpOk);
    CHECK(j.transport.sent.back().bytes == toHost(3, "06 01 00 02 00 01 00 00 00 03"));
    REQUIRE(j.dp->GetPlayerData(j.helm, &data, 0) == kDpOk);
    CHECK(data == hex("03"));

    std::size_t sent = j.transport.sent.size();
    REQUIRE(j.dp->SetPlayerData(kCaptain, hex("77"), kSetLocal) == kDpOk);
    CHECK(j.transport.sent.size() == sent);
    REQUIRE(j.dp->GetPlayerData(kCaptain, &data, kGetLocal) == kDpOk);
    CHECK(data == hex("77"));
    CHECK(j.dp->SetPlayerData(kCaptain, hex("77"), 0) == kDpErrAccessDenied);
}

TEST_CASE("GetPlayerFlags and GetPlayerAddress") {
    WithHelm j;
    Dpid watcher = 0;
    REQUIRE(j.dp->CreatePlayer(&watcher, Name{}, 0, {}, kPlayerSpectator) == kDpOk);
    std::uint32_t flags = 0xFFFFFFFF;
    REQUIRE(j.dp->GetPlayerFlags(j.helm, &flags) == kDpOk);
    CHECK(flags == kPlayerLocal);
    REQUIRE(j.dp->GetPlayerFlags(watcher, &flags) == kDpOk);
    CHECK(flags == (kPlayerLocal | kPlayerSpectator));
    REQUIRE(j.dp->GetPlayerFlags(kCaptain, &flags) == kDpOk);
    CHECK(flags == 0u);

    std::vector<std::uint8_t> address;
    REQUIRE(j.dp->GetPlayerAddress(kCaptain, &address) == kDpOk);
    CHECK(address == tcpIpAddress(0xC000020A, 2300));
    j.transport.localNameResult = platform::Endpoint{0xC0000214, 50001};
    REQUIRE(j.dp->GetPlayerAddress(j.helm, &address) == kDpOk);
    CHECK(address == tcpIpAddress(0xC0000214, 50001));
}

TEST_CASE("DestroyPlayer: only the owner's machine may") {
    WithHelm j;
    CHECK(j.dp->DestroyPlayer(kCaptain) == kDpErrAccessDenied);
    CHECK(j.dp->DestroyPlayer(0x00020009) == kDpErrInvalidPlayer);
    REQUIRE(j.dp->DestroyPlayer(j.helm) == kDpOk);
    REQUIRE(j.transport.sent.size() == 1u);
    CHECK(j.transport.sent[0].bytes == toHost(2, "05 01 00 02 00"));
    Name n;
    CHECK(j.dp->GetPlayerName(j.helm, &n) == kDpErrInvalidPlayer);
}

TEST_CASE("groups: create, add, remove members, destroy") {
    WithHelm j;
    Dpid fleet = 0;
    REQUIRE(j.dp->CreateGroup(&fleet, shortName("Fleet"), {}, 0) == kDpOk);
    CHECK(fleet == 0x00020002u);
    CHECK(j.transport.sent.back().bytes ==
          toHost(2, "04 02 00 02 00 01 00 00 00 00 02 00 00 00 05 00 46 6C 65 65 74 FF FF"
                    "00 00 00 00 00 00"));
    // Helm hears of the group being made.
    std::vector<ReceivedMessage> got = drain(*j.dp);
    REQUIRE(got.size() == 1u);
    CHECK(typeOf(got[0]) == kSysCreatePlayerOrGroup);
    CHECK(got[0].blob.bytes[4] == kPlayerTypeGroup);

    REQUIRE(j.dp->AddPlayerToGroup(fleet, j.helm) == kDpOk);
    CHECK(j.transport.sent.back().bytes == toHost(3, "08 02 00 02 00 01 00 02 00"));
    got = drain(*j.dp);
    REQUIRE(got.size() == 1u);
    CHECK(got[0].blob.bytes == hex("02 00 00 00 02 00 02 00 01 00 02 00"));
    std::size_t sent = j.transport.sent.size();
    REQUIRE(j.dp->AddPlayerToGroup(fleet, j.helm) == kDpOk); // a member already
    CHECK(j.transport.sent.size() == sent);
    REQUIRE(j.dp->AddPlayerToGroup(fleet, kCaptain) == kDpOk);
    drain(*j.dp);

    REQUIRE(j.dp->DeletePlayerFromGroup(fleet, j.helm) == kDpOk);
    CHECK(j.transport.sent.back().bytes == toHost(5, "09 02 00 02 00 01 00 02 00"));
    got = drain(*j.dp);
    REQUIRE(got.size() == 1u);
    CHECK(got[0].blob.bytes == hex("21 00 00 00 02 00 02 00 01 00 02 00"));
    CHECK(j.dp->DeletePlayerFromGroup(fleet, j.helm) == kDpErrInvalidPlayer);

    CHECK(j.dp->AddPlayerToGroup(j.helm, j.helm) == kDpErrInvalidGroup);
    CHECK(j.dp->AddPlayerToGroup(fleet, fleet) == kDpErrInvalidPlayer);
    CHECK(j.dp->DestroyGroup(j.helm) == kDpErrInvalidGroup);
    REQUIRE(j.dp->DestroyGroup(fleet) == kDpOk);
    CHECK(j.transport.sent.back().bytes == toHost(6, "05 02 00 02 00"));
    got = drain(*j.dp);
    REQUIRE(got.size() == 1u);
    CHECK(typeOf(got[0]) == kSysDestroyPlayerOrGroup);
}

TEST_CASE("a remote group's membership changes reach the local players") {
    WithHelm j;
    j.transport.incoming.push_back(
        {mockReliable(2, hex("04 02 00 01 00 01 00 00 00 00 01 00 00 00 FF FF FF FF"
                             "00 00 00 00 00 00")),
         kHostEndpoint});
    j.transport.incoming.push_back(
        {mockReliable(3, hex("08 02 00 01 00 01 00 02 00")), kHostEndpoint});
    j.transport.incoming.push_back(
        {mockReliable(4, hex("08 02 00 01 00 01 00 02 00")), kHostEndpoint}); // again
    j.transport.incoming.push_back(
        {mockReliable(5, hex("09 02 00 01 00 01 00 02 00")), kHostEndpoint});
    std::vector<ReceivedMessage> got = drain(*j.dp);
    REQUIRE(got.size() == 3u);
    CHECK(typeOf(got[0]) == kSysCreatePlayerOrGroup);
    CHECK(got[1].blob.bytes == hex("02 00 00 00 02 00 01 00 01 00 02 00"));
    CHECK(got[2].blob.bytes == hex("21 00 00 00 02 00 01 00 01 00 02 00"));
}

TEST_CASE("a session's player limit counts every machine's players") {
    platform::TransportMock transport;
    FakeClock clock;
    Environment env;
    env.transport = &transport;
    env.clock = &clock;
    DirectPlay dp(env, Lan::testConfig());
    REQUIRE(host(dp, "TestGame", kSessionKeepAlive, 2) == kDpOk);
    Dpid a = 0, b = 0, c = 0;
    REQUIRE(dp.CreatePlayer(&a, Name{}, 0, {}, 0) == kDpOk);
    REQUIRE(dp.CreatePlayer(&b, Name{}, 0, {}, 0) == kDpOk);
    CHECK(dp.CreatePlayer(&c, Name{}, 0, {}, 0) == kDpErrCantCreatePlayer);
    // Groups do not count.
    REQUIRE(dp.CreateGroup(&c, Name{}, {}, 0) == kDpOk);
    // Each local player hears of what is made after it, and never of itself.
    std::vector<ReceivedMessage> got = drain(dp);
    REQUIRE(got.size() == 3u);
    CHECK(got[0].to == a); // b is made
    CHECK(got[1].to == a); // the group, to both
    CHECK(got[2].to == b);
}

} // namespace sfcop::hle::dplay
