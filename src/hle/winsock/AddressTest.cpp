// The byte-order and address functions, on the guest's little-endian view, with hand-written
// values. Addresses are from the RFC 5737 test ranges.
#include "hle/winsock/Winsock.h"

#include <doctest/doctest.h>

#include <string>

namespace sfcop::hle::winsock {

TEST_CASE("htons and ntohs swap the two bytes of a port") {
    CHECK(hostToNetworkShort(0x6914) == 0x1469); // port 26900
    CHECK(networkToHostShort(0x1469) == 0x6914);
    CHECK(hostToNetworkShort(0x0000) == 0x0000);
    CHECK(hostToNetworkShort(0xFF00) == 0x00FF);
    CHECK(sfcop_winsock_htons(0x1A0B) == 0x0B1A);
    CHECK(sfcop_winsock_ntohs(0x0B1A) == 0x1A0B);
}

TEST_CASE("htonl and ntohl reverse the four bytes") {
    CHECK(hostToNetworkLong(0xC0000201) == 0x010200C0); // 192.0.2.1
    CHECK(networkToHostLong(0x010200C0) == 0xC0000201);
    CHECK(hostToNetworkLong(0x7F000001) == 0x0100007F);
    CHECK(sfcop_winsock_htonl(0x11223344) == 0x44332211);
    CHECK(sfcop_winsock_ntohl(0x44332211) == 0x11223344);
}

TEST_CASE("inet_addr reads a dotted quad into an in_addr, first octet in the low byte") {
    CHECK(inetAddr("192.0.2.1") == 0x010200C0);
    CHECK(inetAddr("198.51.100.2") == 0x026433C6);
    CHECK(inetAddr("127.0.0.1") == 0x0100007F);
    CHECK(inetAddr("0.0.0.0") == 0x00000000);
    CHECK(inetAddr("255.255.255.255") == 0xFFFFFFFF);
    CHECK(inetAddr("203.0.113.9 trailing") == 0x097100CB);
    CHECK(sfcop_winsock_inet_addr("192.0.2.1") == 0x010200C0);
}

TEST_CASE("inet_addr reads the short, octal and hex forms") {
    CHECK(inetAddr("192.0.513") == 0x010200C0);  // a.b.c: c is 16 bits
    CHECK(inetAddr("192.513") == 0x010200C0);    // a.b: b is 24 bits
    CHECK(inetAddr("3221225985") == 0x010200C0); // a: 32 bits
    CHECK(inetAddr("0xC0.0.02.1") == 0x010200C0);
    CHECK(inetAddr("0300.0.2.1") == 0x010200C0); // 0300 octal is 192
}

TEST_CASE("inet_addr returns INADDR_NONE for malformed text") {
    CHECK(inetAddr("") == 0xFFFFFFFF);
    CHECK(inetAddr(nullptr) == 0xFFFFFFFF);
    CHECK(inetAddr("192.0.2.256") == 0xFFFFFFFF);
    CHECK(inetAddr("192.0.2") == 0x020000C0); // valid: c = 2
    CHECK(inetAddr("192..2.1") == 0xFFFFFFFF);
    CHECK(inetAddr("192.0.2.1.5") == 0xFFFFFFFF);
    CHECK(inetAddr("192.0.2.x") == 0xFFFFFFFF);
    CHECK(inetAddr("08.0.0.1") == 0xFFFFFFFF); // 8 is not an octal digit
    CHECK(inetAddr("server.test") == 0xFFFFFFFF);
}

TEST_CASE("inet_ntoa writes an in_addr as a dotted quad") {
    CHECK(inetNtoa(0x010200C0) == "192.0.2.1");
    CHECK(inetNtoa(0x026433C6) == "198.51.100.2");
    CHECK(inetNtoa(0x00000000) == "0.0.0.0");
    CHECK(inetNtoa(0xFFFFFFFF) == "255.255.255.255");
    char text[16];
    sfcop_winsock_inet_ntoa(0x097100CB, text);
    CHECK(std::string(text) == "203.0.113.9");
}

} // namespace sfcop::hle::winsock
