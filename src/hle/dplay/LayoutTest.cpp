#include "hle/dplay/Layout.h"

#include "hle/dplay/DplayTestUtil.h"

#include <doctest/doctest.h>

namespace sfcop::hle::dplay {

using test::hex;

TEST_CASE("DPNAME: size, flags, then the short name after the structure") {
    Name name;
    name.shortName = "Helm";
    Blob b = layoutName(name);
    CHECK(b.bytes == hex("10 00 00 00 00 00 00 00 10 00 00 00 00 00 00 00 48 65 6C 6D 00"));
    CHECK(b.pointers == std::vector<std::uint32_t>{8});
}

TEST_CASE("DPSESSIONDESC2: 0x50 bytes, then the name and the password") {
    SessionDesc d;
    d.flags = 0x2044;
    d.instance = makeGuid(0xA1A2A3A4, 0xB1B2, 0xC1C2, {0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8});
    d.application = test::testApplication();
    d.maxPlayers = 8;
    d.currentPlayers = 2;
    d.name = "TestGame";
    d.password = "pw";
    d.user = {1, 2, 3, 4};
    Blob b = layoutSessionDesc(d);
    CHECK(b.bytes == hex("50 00 00 00 44 20 00 00"
                         "A4 A3 A2 A1 B2 B1 C2 C1 D1 D2 D3 D4 D5 D6 D7 D8"
                         "44 33 22 11 66 55 88 77 99 AA BB CC DD EE FF 00"
                         "08 00 00 00 02 00 00 00 50 00 00 00 59 00 00 00"
                         "00 00 00 00 00 00 00 00"
                         "01 00 00 00 02 00 00 00 03 00 00 00 04 00 00 00"
                         "54 65 73 74 47 61 6D 65 00 70 77 00"));
    CHECK(b.pointers == std::vector<std::uint32_t>{48, 52});
}

TEST_CASE("DPMSG_CREATEPLAYERORGROUP: the short name pointer is at +0x20") {
    Name name;
    name.shortName = "Gunner";
    Blob b = layoutCreatePlayerOrGroup(kPlayerTypePlayer, 0x00010002, 3, hex("AA BB"), name, 0x200);
    CHECK(b.bytes == hex("03 00 00 00 01 00 00 00 02 00 01 00 03 00 00 00"
                         "30 00 00 00 02 00 00 00 10 00 00 00 00 00 00 00"
                         "32 00 00 00 00 00 00 00 00 00 00 00 00 02 00 00"
                         "AA BB 47 75 6E 6E 65 72 00"));
    CHECK(b.pointers == std::vector<std::uint32_t>{16, 32});
}

TEST_CASE("DPMSG_DESTROYPLAYERORGROUP: local data, remote data, then the name") {
    Name name;
    name.longName = "Long";
    Blob b = layoutDestroyPlayerOrGroup(kPlayerTypeGroup, 0x00020003, hex("01"), hex("02 03"),
                                        name, 0);
    CHECK(b.bytes == hex("05 00 00 00 00 00 00 00 03 00 02 00 34 00 00 00"
                         "01 00 00 00 35 00 00 00 02 00 00 00 10 00 00 00"
                         "00 00 00 00 00 00 00 00 37 00 00 00 00 00 00 00"
                         "00 00 00 00 01 02 03 4C 6F 6E 67 00"));
    CHECK(b.pointers == std::vector<std::uint32_t>{12, 20, 40});
}

TEST_CASE("the small system messages") {
    CHECK(layoutGroupMembership(kSysAddPlayerToGroup, 0x00010002, 0x00020001).bytes ==
          hex("02 00 00 00 02 00 01 00 01 00 02 00"));
    CHECK(layoutTypeOnly(kSysHost).bytes == hex("01 01 00 00"));
    CHECK(layoutTypeOnly(kSysSessionLost).bytes == hex("31 00 00 00"));
    Blob data = layoutSetPlayerOrGroupData(kPlayerTypePlayer, 0x00010001, hex("DE AD"));
    CHECK(data.bytes == hex("02 01 00 00 01 00 00 00 01 00 01 00 14 00 00 00 02 00 00 00 DE AD"));
    CHECK(data.pointers == std::vector<std::uint32_t>{12});
    Name name;
    name.shortName = "XO";
    Blob named = layoutSetPlayerOrGroupName(kPlayerTypePlayer, 0x00010001, name);
    CHECK(named.bytes == hex("03 01 00 00 01 00 00 00 01 00 01 00"
                             "10 00 00 00 00 00 00 00 1C 00 00 00 00 00 00 00 58 4F 00"));
    CHECK(named.pointers == std::vector<std::uint32_t>{20});
    SessionDesc d;
    Blob desc = layoutSetSessionDesc(d);
    REQUIRE(desc.bytes.size() == 84u);
    CHECK(std::vector<std::uint8_t>(desc.bytes.begin(), desc.bytes.begin() + 8) ==
          hex("04 01 00 00 50 00 00 00"));
    CHECK(desc.pointers.empty());
}

} // namespace sfcop::hle::dplay
