// IDirectPlay4A messages on TransportMock, joined to the mock host's session as peer 2
// (DplayMockFixture.h). The payloads are the game's three frame kinds
// (analysis/netcode/directplay.md): a data bundle (type 1, sent guaranteed with SendEx), a probe
// (type 2, sent with Send) and the probe's reply (type 3). The module carries them as bytes.
#include "hle/dplay/DirectPlay.h"

#include "hle/dplay/DplayMockFixture.h"

#include <doctest/doctest.h>

#include <algorithm>

namespace sfcop::hle::dplay {

using namespace test;

namespace {

constexpr Dpid kCaptain = 0x00010001;
constexpr Dpid kHelm = 0x00020001;

// Type 1, 6 bytes: one record of 4 bytes.
const std::vector<std::uint8_t> kBundle = hex("01 00 00 00 06 00 00 00 04 00 AA BB CC DD");
// Type 2, 12 bytes: sent at 1000 ms, 0, probe 1.
const std::vector<std::uint8_t> kProbe =
    hex("02 00 00 00 0C 00 00 00 E8 03 00 00 00 00 00 00 01 00 00 00");
// Type 3, 12 bytes: the time back, the id the host gives, probe 1.
const std::vector<std::uint8_t> kProbeReply =
    hex("03 00 00 00 0C 00 00 00 E8 03 00 00 01 00 02 00 01 00 00 00");

std::vector<std::uint8_t> toHost(std::uint8_t seq, const std::vector<std::uint8_t> &inner) {
    return concat({hex("53 46 44 50 01 05 00 00 0D F0 AD 0B 02 00 00 00"), {seq, 0, 0, 0}, inner});
}

const std::vector<std::uint8_t> kUnreliableHeader = hex("53 46 44 50 01 07 00 00 0D F0 AD 0B");

// An unreliable datagram the host sends: from `from` to `to`.
std::vector<std::uint8_t> fromHost(Dpid from, Dpid to, const std::vector<std::uint8_t> &payload) {
    std::vector<std::uint8_t> ids = {static_cast<std::uint8_t>(from),
                                     static_cast<std::uint8_t>(from >> 8),
                                     static_cast<std::uint8_t>(from >> 16),
                                     static_cast<std::uint8_t>(from >> 24),
                                     static_cast<std::uint8_t>(to),
                                     static_cast<std::uint8_t>(to >> 8),
                                     static_cast<std::uint8_t>(to >> 16),
                                     static_cast<std::uint8_t>(to >> 24)};
    return concat({kUnreliableHeader, hex("01 00 00 00"), ids, payload});
}

struct WithHelm : MockJoined {
    WithHelm() {
        Dpid helm = 0;
        REQUIRE(dp->CreatePlayer(&helm, Name{}, 0x77, {}, 0) == kDpOk);
        REQUIRE(helm == kHelm);
        transport.sent.clear();
    }
};

} // namespace

TEST_CASE("SendEx guaranteed and async: the bundle on the channel, DPERR_PENDING") {
    WithHelm j;
    std::uint32_t id = 0;
    CHECK(j.dp->SendEx(kHelm, kCaptain, 0x601, kBundle.data(), kBundle.size(), 0, 1000, &id) ==
          kDpErrPending);
    CHECK(id == 1u);
    REQUIRE(j.transport.sent.size() == 1u);
    CHECK(j.transport.sent[0].bytes ==
          toHost(2, concat({hex("0B 01 00 02 00 01 00 01 00"), kBundle})));
    CHECK(j.transport.sent[0].peer == kHostEndpoint);
    // The signed and encrypted forms are guaranteed too.
    CHECK(j.dp->SendEx(kHelm, kCaptain, 0x620, kBundle.data(), kBundle.size(), 0, 1000, &id) ==
          kDpErrPending);
    CHECK(id == 2u);
    CHECK(j.dp->SendEx(kHelm, kCaptain, 0x640, kBundle.data(), kBundle.size(), 0, 1000, &id) ==
          kDpErrPending);
    CHECK(j.transport.sent.size() == 3u);
    CHECK(j.transport.sent[2].bytes[16] == 4); // sequence 4
    CHECK(j.dp->CancelMessage(id, 0) == kDpOk);
}

TEST_CASE("a guaranteed message waits in the send queue until acknowledged") {
    WithHelm j;
    std::uint32_t id = 0;
    REQUIRE(j.dp->SendEx(kHelm, kCaptain, 0x601, kBundle.data(), kBundle.size(), 0, 1000, &id) ==
            kDpErrPending);
    std::uint32_t messages = 0, bytes = 0;
    REQUIRE(j.dp->GetMessageQueue(0, 0, kMessageQueueSend, &messages, &bytes) == kDpOk);
    CHECK(messages == 2u); // Helm's creation, and the bundle
    CHECK(bytes == 14u);
    REQUIRE(j.dp->GetMessageQueue(0, kCaptain, 0, &messages, &bytes) == kDpOk);
    CHECK(messages == 2u);

    // Not acknowledged: both are sent again after 200 ms, unchanged.
    j.clock.Advance(200);
    j.dp->Service(0);
    REQUIRE(j.transport.sent.size() == 3u);
    CHECK(j.transport.sent[1].bytes[16] == 1);
    CHECK(j.transport.sent[2].bytes == j.transport.sent[0].bytes);

    j.transport.incoming.push_back(
        {hex("53 46 44 50 01 06 00 00 0D F0 AD 0B 01 00 00 00 02 00 00 00"), kHostEndpoint});
    REQUIRE(j.dp->GetMessageQueue(0, 0, kMessageQueueSend, &messages, &bytes) == kDpOk);
    CHECK(messages == 0u);
    CHECK(bytes == 0u);
    j.clock.Advance(200);
    j.dp->Service(0);
    CHECK(j.transport.sent.size() == 3u);
    CHECK(j.dp->GetMessageQueue(0, 0, kMessageQueueSend | kMessageQueueReceive, &messages,
                                &bytes) == kDpErrInvalidFlags);
}

TEST_CASE("Send unguaranteed: the probe in one datagram, nothing queued") {
    WithHelm j;
    REQUIRE(j.dp->Send(kHelm, kCaptain, 0, kProbe.data(), kProbe.size()) == kDpOk);
    REQUIRE(j.transport.sent.size() == 1u);
    CHECK(j.transport.sent[0].bytes ==
          concat({kUnreliableHeader, hex("02 00 00 00 01 00 02 00 01 00 01 00"), kProbe}));
    // Helm's creation is the only thing waiting for an acknowledgement.
    std::uint32_t messages = 0;
    REQUIRE(j.dp->GetMessageQueue(0, 0, kMessageQueueSend, &messages, nullptr) == kDpOk);
    CHECK(messages == 1u);
    // Send never answers DPERR_PENDING.
    CHECK(j.dp->Send(kHelm, kCaptain, kSendAsync, kProbe.data(), kProbe.size()) == kDpOk);
}

TEST_CASE("Receive: the probe reply as sent, after a size query") {
    WithHelm j;
    j.transport.incoming.push_back({fromHost(kCaptain, kHelm, kProbeReply), kHostEndpoint});
    Dpid from = 0, to = 0;
    std::uint32_t size = 4;
    ReceivedMessage m;
    CHECK(j.dp->Receive(&from, &to, kReceiveAll, &size, &m) == kDpErrBufferTooSmall);
    CHECK(size == 20u);
    // The game grows its buffer and asks again, without resetting the size.
    REQUIRE(j.dp->Receive(&from, &to, kReceiveAll, &size, &m) == kDpOk);
    CHECK(size == 20u);
    CHECK(from == kCaptain);
    CHECK(to == kHelm);
    CHECK(m.blob.bytes == kProbeReply);
    CHECK(m.blob.pointers.empty());
    CHECK(j.dp->Receive(&from, &to, kReceiveAll, &size, &m) == kDpErrNoMessages);
    CHECK(j.dp->Receive(nullptr, &to, kReceiveAll, &size, &m) == kDpErrInvalidParams);
}

TEST_CASE("Receive: a guaranteed bundle, in order, and the player's event") {
    WithHelm j;
    j.transport.incoming.push_back(
        {mockReliable(2, concat({hex("0B 01 00 01 00 01 00 02 00"), kBundle})), kHostEndpoint});
    j.transport.incoming.push_back({fromHost(kCaptain, kHelm, kProbe), kHostEndpoint});
    std::vector<ReceivedMessage> got = drain(*j.dp);
    REQUIRE(got.size() == 2u);
    CHECK(got[0].blob.bytes == kBundle);
    CHECK(got[1].blob.bytes == kProbe);
    CHECK(j.signalled == std::vector<std::uint32_t>{0x77, 0x77});
}

TEST_CASE("Receive with PEEK, TOPLAYER and FROMPLAYER") {
    WithHelm j;
    Dpid gunner = 0;
    REQUIRE(j.dp->CreatePlayer(&gunner, Name{}, 0, {}, 0) == kDpOk);
    drain(*j.dp); // Helm hears of the gunner
    j.transport.incoming.push_back({fromHost(kCaptain, kHelm, kProbe), kHostEndpoint});
    j.transport.incoming.push_back({fromHost(kCaptain, gunner, kProbeReply), kHostEndpoint});

    std::uint32_t count = 0;
    REQUIRE(j.dp->GetMessageCount(gunner, &count) == kDpOk);
    CHECK(count == 1u);
    CHECK(j.dp->GetMessageCount(kCaptain, &count) == kDpErrInvalidPlayer);
    std::uint32_t messages = 0, bytes = 0;
    REQUIRE(j.dp->GetMessageQueue(0, 0, kMessageQueueReceive, &messages, &bytes) == kDpOk);
    CHECK(messages == 2u);
    CHECK(bytes == 40u);
    REQUIRE(j.dp->GetMessageQueue(0, gunner, kMessageQueueReceive, &messages, &bytes) == kDpOk);
    CHECK(messages == 1u);

    Dpid from = 0, to = gunner;
    std::uint32_t size = 100;
    ReceivedMessage m;
    REQUIRE(j.dp->Receive(&from, &to, kReceiveToPlayer | kReceivePeek, &size, &m) == kDpOk);
    CHECK(m.blob.bytes == kProbeReply);
    to = gunner;
    REQUIRE(j.dp->Receive(&from, &to, kReceiveToPlayer, &size, &m) == kDpOk);
    CHECK(m.blob.bytes == kProbeReply);
    to = gunner;
    CHECK(j.dp->Receive(&from, &to, kReceiveToPlayer, &size, &m) == kDpErrNoMessages);
    from = 0x00010009;
    CHECK(j.dp->Receive(&from, &to, kReceiveFromPlayer, &size, &m) == kDpErrNoMessages);
    from = kCaptain;
    REQUIRE(j.dp->Receive(&from, &to, kReceiveFromPlayer, &size, &m) == kDpOk);
    CHECK(to == kHelm);
    CHECK(m.blob.bytes == kProbe);
}

TEST_CASE("sending to every player and to a group reaches local players too") {
    WithHelm j;
    Dpid gunner = 0, fleet = 0;
    REQUIRE(j.dp->CreatePlayer(&gunner, Name{}, 0, {}, 0) == kDpOk);
    REQUIRE(j.dp->CreateGroup(&fleet, Name{}, {}, 0) == kDpOk);
    REQUIRE(j.dp->AddPlayerToGroup(fleet, kHelm) == kDpOk);
    REQUIRE(j.dp->AddPlayerToGroup(fleet, gunner) == kDpOk);
    drain(*j.dp);
    j.transport.sent.clear();

    REQUIRE(j.dp->Send(kHelm, kDpidAllPlayers, 0, kProbe.data(), kProbe.size()) == kDpOk);
    REQUIRE(j.transport.sent.size() == 1u); // the host's machine, once
    CHECK(j.transport.sent[0].bytes ==
          concat({kUnreliableHeader, hex("02 00 00 00 01 00 02 00 00 00 00 00"), kProbe}));
    std::vector<ReceivedMessage> got = drain(*j.dp);
    REQUIRE(got.size() == 1u); // the gunner; never the sender
    CHECK(got[0].to == gunner);
    CHECK(got[0].from == kHelm);

    j.transport.sent.clear();
    REQUIRE(j.dp->Send(kHelm, fleet, 0, kProbe.data(), kProbe.size()) == kDpOk);
    CHECK(j.transport.sent.empty()); // no member is on another machine
    got = drain(*j.dp);
    REQUIRE(got.size() == 1u); // to the gunner, as the member it reached
    CHECK(got[0].to == gunner);
}

TEST_CASE("Send refuses a remote sender, an unknown target and an oversized message") {
    WithHelm j;
    CHECK(j.dp->Send(kCaptain, kHelm, 0, kProbe.data(), kProbe.size()) == kDpErrInvalidPlayer);
    CHECK(j.dp->Send(kHelm, 0x00050001, 0, kProbe.data(), kProbe.size()) == kDpErrInvalidPlayer);
    std::vector<std::uint8_t> big(60001);
    CHECK(j.dp->Send(kHelm, kCaptain, 0, big.data(), big.size()) == kDpErrSendTooBig);
    big.resize(60000);
    CHECK(j.dp->Send(kHelm, kCaptain, 0, big.data(), big.size()) == kDpOk);
    CHECK(j.dp->Send(kHelm, kCaptain, 0, nullptr, 3) == kDpErrInvalidParams);
    CHECK(j.dp->Send(kHelm, kCaptain, 0, nullptr, 0) == kDpOk);
}

TEST_CASE("a quiet peer gets a keepalive each second") {
    WithHelm j;
    j.transport.incoming.push_back(
        {hex("53 46 44 50 01 06 00 00 0D F0 AD 0B 01 00 00 00 01 00 00 00"), kHostEndpoint});
    j.dp->Service(0);
    j.transport.sent.clear();
    j.clock.Advance(999);
    j.dp->Service(0);
    CHECK(j.transport.sent.empty());
    j.clock.Advance(1);
    j.dp->Service(0);
    REQUIRE(j.transport.sent.size() == 1u);
    CHECK(j.transport.sent[0].bytes == hex("53 46 44 50 01 08 00 00 0D F0 AD 0B 02 00 00 00"));
    // A keepalive from the host is all it takes to be heard from.
    j.clock.Advance(9000);
    j.transport.incoming.push_back(
        {hex("53 46 44 50 01 08 00 00 0D F0 AD 0B 01 00 00 00"), kHostEndpoint});
    j.dp->Service(0);
    j.clock.Advance(9000);
    j.dp->Service(0);
    Name n;
    CHECK(j.dp->GetPlayerName(kCaptain, &n) == kDpOk);
}

TEST_CASE("a host silent for ten seconds is gone; in a migrating session this machine hosts") {
    WithHelm j;
    j.clock.Advance(9990);
    j.dp->Service(0);
    CHECK(drain(*j.dp).empty());
    j.transport.log.Clear();
    j.clock.Advance(10);
    j.dp->Service(0);
    std::vector<ReceivedMessage> got = drain(*j.dp);
    REQUIRE(got.size() == 2u);
    CHECK(typeOf(got[0]) == kSysDestroyPlayerOrGroup);
    CHECK(got[0].blob.bytes[8] == 0x01); // the captain, 0x00010001
    CHECK(got[1].blob.bytes == hex("01 01 00 00")); // DPSYS_HOST
    const std::vector<std::string> &calls = j.transport.log.Calls();
    CHECK(std::find(calls.begin(), calls.end(), "Bind(1, 0, 47700)") != calls.end());
    Caps caps;
    REQUIRE(j.dp->GetCaps(&caps, 0) == kDpOk);
    CHECK((caps.flags & kCapsIsHost) != 0u);
    SessionDesc d;
    REQUIRE(j.dp->GetSessionDesc(&d) == kDpOk);
    CHECK(d.instance == mockInstance());
    CHECK(d.currentPlayers == 1u);
}

TEST_CASE("a host that leaves is gone at once") {
    WithHelm j;
    j.transport.incoming.push_back(
        {hex("53 46 44 50 01 09 00 00 0D F0 AD 0B 01 00 00 00"), kHostEndpoint});
    std::vector<ReceivedMessage> got = drain(*j.dp);
    REQUIRE(got.size() == 2u);
    CHECK(typeOf(got[0]) == kSysDestroyPlayerOrGroup);
    CHECK(typeOf(got[1]) == kSysHost);
    CHECK(j.dp->Send(kHelm, kCaptain, 0, kProbe.data(), kProbe.size()) == kDpErrInvalidPlayer);
}

} // namespace sfcop::hle::dplay
