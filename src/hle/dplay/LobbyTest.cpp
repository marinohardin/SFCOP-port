#include "hle/dplay/Lobby.h"

#include "hle/dplay/DplayTestUtil.h"

#include <doctest/doctest.h>

namespace sfcop::hle::dplay {

using test::hex;

TEST_CASE("CreateCompoundAddress builds what InitializeConnection takes") {
    Lobby lobby;
    std::vector<std::uint8_t> sp(kSpTcpIp.bytes.begin(), kSpTcpIp.bytes.end());
    std::vector<std::uint8_t> inet = hex("31 39 32 2E 30 2E 32 2E 31 30 00");
    std::vector<std::uint8_t> out;
    REQUIRE(lobby.CreateCompoundAddress({{kAidServiceProvider, sp}, {kAidINet, inet}}, &out) ==
            kDpOk);
    std::optional<std::vector<AddressElement>> elements = parseAddress(out.data(), out.size());
    REQUIRE(elements);
    REQUIRE(elements->size() == 3u);
    CHECK((*elements)[0] == AddressElement{kAidTotalSize, hex("5B 00 00 00")});
    CHECK((*elements)[1] == AddressElement{kAidServiceProvider, sp});
    CHECK((*elements)[2] == AddressElement{kAidINet, inet});
    CHECK(lobby.CreateCompoundAddress({}, &out) == kDpErrInvalidParams);
}

TEST_CASE("CreateAddress: the service provider and one chunk") {
    Lobby lobby;
    std::vector<std::uint8_t> port = hex("FC 08");
    std::vector<std::uint8_t> out;
    REQUIRE(lobby.CreateAddress(kSpTcpIp, kAidINetPort, port.data(), port.size(), &out) == kDpOk);
    CHECK(out.size() == 24u + 36u + 22u);
    std::optional<std::vector<AddressElement>> elements = parseAddress(out.data(), out.size());
    REQUIRE(elements);
    CHECK(tcpIpTargetOf(*elements).port == std::optional<std::uint16_t>(2300));
}

TEST_CASE("the size query: a null buffer or a short one gets the size and BUFFERTOOSMALL") {
    std::vector<std::uint8_t> bytes = hex("01 02 03");
    std::uint32_t size = 0;
    CHECK(copyOut(bytes, nullptr, &size) == kDpErrBufferTooSmall);
    CHECK(size == 3u);
    std::uint8_t two[2] = {};
    size = 2;
    CHECK(copyOut(bytes, two, &size) == kDpErrBufferTooSmall);
    CHECK(size == 3u);
    std::uint8_t room[8] = {};
    size = 8;
    CHECK(copyOut(bytes, room, &size) == kDpOk);
    CHECK(size == 3u);
    CHECK(room[0] == 1);
    CHECK(room[2] == 3);
    CHECK(copyOut(bytes, room, nullptr) == kDpErrInvalidParams);
}

TEST_CASE("EnumAddress reports every chunk until the callback stops") {
    Lobby lobby;
    std::vector<std::uint8_t> address = tcpIpAddress(0xC000020A, 2300);
    std::vector<Guid> seen;
    REQUIRE(lobby.EnumAddress(address.data(), address.size(), [&](const AddressElement &e) {
        seen.push_back(e.type);
        return true;
    }) == kDpOk);
    CHECK(seen == std::vector<Guid>{kAidTotalSize, kAidServiceProvider, kAidINet, kAidINetPort});
    seen.clear();
    REQUIRE(lobby.EnumAddress(address.data(), address.size(), [&](const AddressElement &e) {
        seen.push_back(e.type);
        return seen.size() < 2;
    }) == kDpOk);
    CHECK(seen.size() == 2u);
    CHECK(lobby.EnumAddress(address.data(), 3, [](const AddressElement &) { return true; }) ==
          kDpErrInvalidParams);
}

TEST_CASE("a game not started by a lobby: the lobby-launch methods say so") {
    Lobby lobby;
    CHECK(lobby.GetConnectionSettings() == kDpErrNotLobbied);
    CHECK(lobby.SetConnectionSettings() == kDpErrNotLobbied);
    CHECK(lobby.Connect() == kDpErrNotLobbied);
    CHECK(lobby.ConnectEx() == kDpErrNotLobbied);
    CHECK(lobby.WaitForConnectionSettings() == kDpErrNotLobbied);
    CHECK(lobby.RunApplication() == kDpErrUnsupported);
    CHECK(lobby.AddRef() == 2u);
    CHECK(lobby.Release() == 1u);
}

} // namespace sfcop::hle::dplay
