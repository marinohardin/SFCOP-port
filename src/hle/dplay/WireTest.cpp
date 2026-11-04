#include "hle/dplay/Wire.h"

#include "hle/dplay/DplayMockFixture.h"

#include <doctest/doctest.h>

namespace sfcop::hle::dplay::wire {

using test::concat;
using test::hex;

namespace {

std::optional<Packet> decode(const std::vector<std::uint8_t> &bytes) {
    return decodePacket(bytes.data(), bytes.size());
}

} // namespace

TEST_CASE("an enumeration request: header, request id, application") {
    CHECK(encodeEnumRequest(7, test::testApplication()) ==
          hex("53 46 44 50 01 01 00 00 07 00 00 00"
              "44 33 22 11 66 55 88 77 99 AA BB CC DD EE FF 00"));
}

TEST_CASE("an enumeration reply hides the password and says one is needed") {
    SessionDesc d;
    d.flags = 0x2044;
    d.instance = test::mockInstance();
    d.application = test::testApplication();
    d.maxPlayers = 8;
    d.currentPlayers = 1;
    d.user = {1, 2, 3, 4};
    d.name = "TestGame";
    CHECK(encodeEnumReply(1, d) == test::mockEnumReply(1));
    d.password = "pw";
    std::vector<std::uint8_t> withPassword = encodeEnumReply(1, d);
    CHECK(withPassword.size() == test::mockEnumReply(1).size());
    CHECK(std::vector<std::uint8_t>(withPassword.begin() + 12, withPassword.begin() + 16) ==
          hex("44 24 00 00"));
    std::optional<Packet> p = decode(withPassword);
    REQUIRE(p);
    CHECK(p->desc.flags == 0x2444u);
    CHECK_FALSE(p->desc.password);
}

TEST_CASE("a join request and a join reject") {
    std::vector<std::uint8_t> instance = test::guidBytes(test::mockInstance());
    CHECK(encodeJoinRequest(test::mockInstance(), std::string("pw")) ==
          concat({hex("53 46 44 50 01 03 00 00"), instance, hex("02 00 70 77")}));
    CHECK(encodeJoinRequest(test::mockInstance(), std::nullopt) ==
          concat({hex("53 46 44 50 01 03 00 00"), instance, hex("FF FF")}));
    CHECK(encodeJoinReject(test::mockInstance(), kDpErrInvalidPassword) ==
          concat({hex("53 46 44 50 01 04 00 00"), instance, hex("54 01 77 88")}));
}

TEST_CASE("peer packets: key and sender after the header") {
    CHECK(encodeAck(0x0BADF00D, 2, 5) ==
          hex("53 46 44 50 01 06 00 00 0D F0 AD 0B 02 00 00 00 05 00 00 00"));
    CHECK(encodeKeepalive(0x0BADF00D, 2) ==
          hex("53 46 44 50 01 08 00 00 0D F0 AD 0B 02 00 00 00"));
    CHECK(encodeLeave(0x0BADF00D, 2) == hex("53 46 44 50 01 09 00 00 0D F0 AD 0B 02 00 00 00"));
    std::vector<std::uint8_t> payload = hex("02 00 00 00 0C 00 00 00 E8 03 00 00 00 00 00 00 01 00 00 00");
    CHECK(encodeUnreliable(0x0BADF00D, 2, 0x00020001, 0x00010001, payload.data(), payload.size()) ==
          concat({hex("53 46 44 50 01 07 00 00 0D F0 AD 0B 02 00 00 00 01 00 02 00 01 00 01 00"),
                  payload}));
    CHECK(encodeReliable(0x0BADF00D, 1, 3, hex("05 01 00 01 00")) ==
          hex("53 46 44 50 01 05 00 00 0D F0 AD 0B 01 00 00 00 03 00 00 00 05 01 00 01 00"));
}

TEST_CASE("the mock host's join accept decodes to the session and its player") {
    std::vector<std::uint8_t> datagram = test::mockJoinAccept();
    std::optional<Packet> p = decode(datagram);
    REQUIRE(p);
    CHECK(p->type == PacketType::kReliable);
    CHECK(p->sessionKey == 0x0BADF00Du);
    CHECK(p->sender == 1u);
    CHECK(p->seq == 1u);
    std::optional<Inner> m = decodeInner(p->body);
    REQUIRE(m);
    CHECK(m->type == InnerType::kJoinAccept);
    CHECK(m->yourPeer == 2u);
    CHECK(m->nextPeer == 3u);
    CHECK(m->desc.name == std::optional<std::string>("TestGame"));
    CHECK(m->desc.maxPlayers == 8u);
    CHECK(m->peers.empty());
    REQUIRE(m->entities.size() == 1u);
    const Entity &e = m->entities[0];
    CHECK(e.id == 0x00010001u);
    CHECK_FALSE(e.group);
    CHECK(e.owner == 1u);
    CHECK(e.name.shortName == std::optional<std::string>("TestCaptain"));
    CHECK_FALSE(e.name.longName);
    CHECK(e.data == hex("DE AD BE EF"));
}

TEST_CASE("every inner message encodes by hand") {
    Entity group;
    group.id = 0x00020002;
    group.group = true;
    group.owner = 2;
    group.name.shortName = "Fleet";
    group.members = {0x00020001};
    CHECK(innerEntityAdd(group) == hex("04 02 00 02 00 01 00 00 00 00 02 00 00 00"
                                       "05 00 46 6C 65 65 74 FF FF 00 00 00 00 01 00 01 00 02 00"));
    CHECK(innerEntityRemove(0x00020002) == hex("05 02 00 02 00"));
    CHECK(innerPeerAdd(PeerInfo{3, 0xC0000203, 50000}) == hex("02 03 00 00 00 03 02 00 C0 50 C3"));
    CHECK(innerPeerRemove(3) == hex("03 03 00 00 00"));
    CHECK(innerSetData(0x00020001, hex("AA")) == hex("06 01 00 02 00 01 00 00 00 AA"));
    Name name;
    name.shortName = "XO";
    CHECK(innerSetName(0x00020001, name) == hex("07 01 00 02 00 02 00 58 4F FF FF"));
    CHECK(innerMember(true, 0x00020002, 0x00020001) == hex("08 02 00 02 00 01 00 02 00"));
    CHECK(innerMember(false, 0x00020002, 0x00020001) == hex("09 02 00 02 00 01 00 02 00"));
    std::vector<std::uint8_t> payload = hex("01 00 00 00 00 00 00 00");
    CHECK(innerData(0x00020001, 0, payload.data(), payload.size()) ==
          hex("0B 01 00 02 00 00 00 00 00 01 00 00 00 00 00 00 00"));
}

TEST_CASE("decoding refuses what is not exactly one message") {
    std::vector<std::uint8_t> ack = hex("53 46 44 50 01 06 00 00 0D F0 AD 0B 02 00 00 00 05 00 00 00");
    REQUIRE(decode(ack));
    CHECK_FALSE(decode(concat({ack, hex("00")})));                      // trailing byte
    CHECK_FALSE(decode(std::vector<std::uint8_t>(ack.begin(), ack.end() - 1))); // short
    std::vector<std::uint8_t> magic = ack;
    magic[0] = 0x54;
    CHECK_FALSE(decode(magic));
    std::vector<std::uint8_t> version = ack;
    version[4] = 2;
    CHECK_FALSE(decode(version));
    std::vector<std::uint8_t> type = ack;
    type[5] = 0x0A;
    CHECK_FALSE(decode(type));
    CHECK_FALSE(decode(hex("53 46 44 50 01 05 00 00 0D F0 AD 0B 01 00 00 00 03 00 00 00")));
    CHECK_FALSE(decode({}));
    // A string that runs past the end.
    CHECK_FALSE(decode(concat({hex("53 46 44 50 01 03 00 00"),
                               test::guidBytes(test::mockInstance()), hex("05 00 70 77")})));
}

TEST_CASE("decoding an inner message refuses bad counts and trailing bytes") {
    CHECK(decodeInner(hex("05 02 00 02 00")));
    CHECK_FALSE(decodeInner(hex("05 02 00 02 00 00")));
    CHECK_FALSE(decodeInner(hex("0C 00 00 00 00")));
    CHECK_FALSE(decodeInner({}));
    // An entity whose group byte is neither 0 nor 1.
    CHECK_FALSE(decodeInner(hex("04 01 00 02 00 02 00 00 00 00 02 00 00 00 FF FF FF FF"
                                "00 00 00 00 00 00")));
    // A member count past the cap.
    CHECK_FALSE(decodeInner(hex("04 01 00 02 00 01 00 00 00 00 02 00 00 00 FF FF FF FF"
                                "00 00 00 00 01 10")));
}

} // namespace sfcop::hle::dplay::wire
