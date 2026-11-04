#include "hle/dplay/Address.h"

#include "hle/dplay/DplayTestUtil.h"

#include <doctest/doctest.h>

namespace sfcop::hle::dplay {

using test::hex;

namespace {

// DPAID_TotalSize, DPAID_ServiceProvider, DPAID_INet and DPAID_INetPort as bytes.
const char kTotalSize[] = "60 F5 18 13 2C 91 D0 11 9D AA 00 A0 C9 0A 43 CB";
const char kServiceProvider[] = "C0 16 D9 07 AF E0 CF 11 9C 4E 00 A0 C9 05 42 5E";
const char kINet[] = "A0 4D A5 C4 AF E0 CF 11 9C 4E 00 A0 C9 05 42 5E";
const char kINetPort[] = "41 45 52 E4 A5 8E D1 11 8A 96 00 60 97 B0 14 11";
const char kTcpIp[] = "E0 5E E9 36 77 85 CF 11 96 0C 00 80 C7 53 4E 82";

} // namespace

TEST_CASE("the TCP/IP connection is a total size chunk and the service provider") {
    std::vector<std::uint8_t> expected = test::concat(
        {hex(kTotalSize), hex("04 00 00 00 3C 00 00 00"), hex(kServiceProvider),
         hex("10 00 00 00"), hex(kTcpIp)});
    CHECK(tcpIpConnection() == expected);
}

TEST_CASE("a player's address names the service provider, the dotted quad and the port") {
    std::vector<std::uint8_t> expected = test::concat(
        {hex(kTotalSize), hex("04 00 00 00 71 00 00 00"), hex(kServiceProvider),
         hex("10 00 00 00"), hex(kTcpIp), hex(kINet), hex("0B 00 00 00"),
         hex("31 39 32 2E 30 2E 32 2E 31 30 00"), hex(kINetPort), hex("02 00 00 00 FC 08")});
    CHECK(tcpIpAddress(0xC000020A, 2300) == expected);
}

TEST_CASE("parsing an address gives every chunk and its target") {
    std::vector<std::uint8_t> address = tcpIpAddress(0xC000020A, 2300);
    std::optional<std::vector<AddressElement>> elements =
        parseAddress(address.data(), address.size());
    REQUIRE(elements);
    REQUIRE(elements->size() == 4u);
    CHECK((*elements)[0].type == kAidTotalSize);
    CHECK(serviceProviderOf(*elements) == kSpTcpIp);
    TcpIpTarget target = tcpIpTargetOf(*elements);
    CHECK(target.host == "192.0.2.10");
    CHECK(target.port == std::optional<std::uint16_t>(2300));
}

TEST_CASE("a connection with no INet chunk searches the network") {
    std::vector<std::uint8_t> address = tcpIpConnection();
    std::optional<std::vector<AddressElement>> elements =
        parseAddress(address.data(), address.size());
    REQUIRE(elements);
    TcpIpTarget target = tcpIpTargetOf(*elements);
    CHECK(target.host.empty());
    CHECK_FALSE(target.port);
}

TEST_CASE("an address whose chunk runs past the end is refused") {
    std::vector<std::uint8_t> address = tcpIpConnection();
    CHECK_FALSE(parseAddress(address.data(), address.size() - 1));
    CHECK_FALSE(parseAddress(address.data(), 0));
    CHECK_FALSE(parseAddress(nullptr, 0));
    std::vector<std::uint8_t> huge = test::concat({hex(kTotalSize), hex("FF FF FF FF")});
    CHECK_FALSE(parseAddress(huge.data(), huge.size()));
}

TEST_CASE("buildAddress puts one total size chunk first") {
    std::vector<std::uint8_t> port = hex("FC 08");
    std::vector<std::uint8_t> built =
        buildAddress({{kAidTotalSize, hex("00 00 00 00")}, {kAidINetPort, port}});
    std::vector<std::uint8_t> expected =
        test::concat({hex(kTotalSize), hex("04 00 00 00 2E 00 00 00"), hex(kINetPort),
                      hex("02 00 00 00 FC 08")});
    CHECK(built == expected);
}

TEST_CASE("dotted quads") {
    CHECK(dottedQuad(0xC0000201) == "192.0.2.1");
    CHECK(parseDottedQuad("198.51.100.7") == std::optional<std::uint32_t>(0xC6336407));
    CHECK(parseDottedQuad("0.0.0.0") == std::optional<std::uint32_t>(0));
    CHECK_FALSE(parseDottedQuad("256.0.0.1"));
    CHECK_FALSE(parseDottedQuad("1.2.3"));
    CHECK_FALSE(parseDottedQuad("1.2.3.4.5"));
    CHECK_FALSE(parseDottedQuad("1.2.3.4 "));
    CHECK_FALSE(parseDottedQuad("host.example"));
    CHECK_FALSE(parseDottedQuad("0001.2.3.4"));
    CHECK_FALSE(parseDottedQuad(""));
}

} // namespace sfcop::hle::dplay
