// select and __WSAFDIsSet: the fd_set translation, the timeout conversion and a non-blocking
// connect's end, through the Transport mock.
#include "hle/winsock/Winsock.h"
#include "platform/Transport/TransportMock.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace sfcop::hle::winsock {
namespace {

using platform::Error;
using platform::SocketId;
using platform::TransportMock;

struct Fixture {
    TransportMock mock;
    Winsock winsock{mock};
    Fixture() {
        WsaData data{};
        REQUIRE(winsock.WsaStartup(0x0101, &data) == 0);
        Winsock::WsaSetLastError(0);
    }
    std::uint32_t open(std::int32_t type, std::uint32_t id) {
        mock.openResult = SocketId{id};
        std::uint32_t s = winsock.Socket(kAfInet, type, 0);
        REQUIRE(s != kInvalidSocket);
        return s;
    }
};

FdSet setOf(std::initializer_list<std::uint32_t> sockets) {
    FdSet set{};
    for (std::uint32_t s : sockets) {
        set.sockets[set.count++] = s;
    }
    return set;
}

} // namespace

TEST_CASE("select keeps only the ready sockets of each set, in order") {
    Fixture f;
    std::uint32_t a = f.open(kSockDgram, 7);
    std::uint32_t b = f.open(kSockDgram, 8);
    std::uint32_t c = f.open(kSockStream, 9);
    f.mock.log.Clear();
    f.mock.selectReady.readable = {SocketId{9}, SocketId{7}};
    f.mock.selectReady.writable = {SocketId{8}};
    FdSet readfds = setOf({a, b, c});
    FdSet writefds = setOf({b});
    TimeVal timeout{1, 500000};
    CHECK(f.winsock.Select(0, &readfds, &writefds, nullptr, &timeout) == 3);
    CHECK(readfds.count == 2);
    CHECK(readfds.sockets[0] == a);
    CHECK(readfds.sockets[1] == c);
    CHECK(writefds.count == 1);
    CHECK(writefds.sockets[0] == b);
    CHECK(f.mock.log.Calls() == std::vector<std::string>{"Select(3, 1, 0, 1500)"});
}

TEST_CASE("select converts the timeout: NULL waits forever, zero polls, microseconds round up") {
    Fixture f;
    std::uint32_t a = f.open(kSockDgram, 7);
    f.mock.log.Clear();
    FdSet readfds = setOf({a});
    CHECK(f.winsock.Select(0, &readfds, nullptr, nullptr, nullptr) == 0);
    readfds = setOf({a});
    TimeVal zero{0, 0};
    CHECK(f.winsock.Select(0, &readfds, nullptr, nullptr, &zero) == 0);
    readfds = setOf({a});
    TimeVal tiny{0, 1};
    CHECK(f.winsock.Select(0, &readfds, nullptr, nullptr, &tiny) == 0);
    readfds = setOf({a});
    TimeVal wait{0, 200000};
    CHECK(f.winsock.Select(0, &readfds, nullptr, nullptr, &wait) == 0);
    CHECK(readfds.count == 0);
    CHECK(f.mock.log.Calls() == std::vector<std::string>{
                                    "Select(1, 0, 0, \"forever\")",
                                    "Select(1, 0, 0, 0)",
                                    "Select(1, 0, 0, 1)",
                                    "Select(1, 0, 0, 200)",
                                });
}

TEST_CASE("select error paths") {
    Fixture f;
    std::uint32_t a = f.open(kSockDgram, 7);
    FdSet none{};
    CHECK(f.winsock.Select(0, &none, nullptr, nullptr, nullptr) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEInval);
    CHECK(f.winsock.Select(0, nullptr, nullptr, nullptr, nullptr) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEInval);
    FdSet unknown = setOf({a, 0x7777});
    CHECK(f.winsock.Select(0, &unknown, nullptr, nullptr, nullptr) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaENotSock);
    CHECK(unknown.count == 2); // left as it was
    FdSet readfds = setOf({a});
    TimeVal negative{-1, 0};
    CHECK(f.winsock.Select(0, &readfds, nullptr, nullptr, &negative) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEInval);
    f.mock.selectError = Error::kNotConnected;
    CHECK(f.winsock.Select(0, &readfds, nullptr, nullptr, nullptr) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEIntr);
    f.winsock.WsaCleanup();
    CHECK(f.winsock.Select(0, &readfds, nullptr, nullptr, nullptr) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaNotInitialised);
}

TEST_CASE("a non-blocking connect that select reports writable has succeeded") {
    Fixture f;
    std::uint32_t s = f.open(kSockStream, 7);
    f.mock.connectResult = Error::kInProgress;
    SockAddrIn peer{kAfInet, 6667, 0xC0000201};
    REQUIRE(f.winsock.Connect(s, &peer, 16) == kSocketError);
    REQUIRE(Winsock::WsaGetLastError() == kWsaEWouldBlock);
    // The game's pattern: wait on writefds and exceptfds, then read SO_ERROR.
    f.mock.selectReady.writable = {SocketId{7}};
    FdSet writefds = setOf({s});
    FdSet exceptfds = setOf({s});
    TimeVal timeout{5, 0};
    CHECK(f.winsock.Select(64, nullptr, &writefds, &exceptfds, &timeout) == 1);
    CHECK(fdIsSet(s, &writefds) == 1);
    CHECK(fdIsSet(s, &exceptfds) == 0);
    std::uint32_t error = 0xFFFF;
    std::int32_t length = 4;
    CHECK(f.winsock.GetSockOpt(s, kSolSocket, kSoError, &error, &length) == 0);
    CHECK(error == 0);
}

TEST_CASE("a non-blocking connect that select reports in exceptfds has failed") {
    Fixture f;
    std::uint32_t s = f.open(kSockStream, 7);
    f.mock.connectResult = Error::kInProgress;
    SockAddrIn peer{kAfInet, 6667, 0xC0000201};
    REQUIRE(f.winsock.Connect(s, &peer, 16) == kSocketError);
    f.mock.selectReady.errored = {SocketId{7}};
    FdSet writefds = setOf({s});
    FdSet exceptfds = setOf({s});
    TimeVal timeout{5, 0};
    CHECK(f.winsock.Select(64, nullptr, &writefds, &exceptfds, &timeout) == 1);
    CHECK(fdIsSet(s, &writefds) == 0);
    CHECK(fdIsSet(s, &exceptfds) == 1);
    std::uint32_t error = 0;
    std::int32_t length = 4;
    CHECK(f.winsock.GetSockOpt(s, kSolSocket, kSoError, &error, &length) == 0);
    CHECK(error == 10061);
}

TEST_CASE("__WSAFDIsSet finds a socket among the set's first count entries only") {
    FdSet set = setOf({0x100, 0x104});
    set.sockets[2] = 0x108; // beyond count
    CHECK(fdIsSet(0x100, &set) == 1);
    CHECK(fdIsSet(0x104, &set) == 1);
    CHECK(fdIsSet(0x108, &set) == 0);
    CHECK(fdIsSet(0x100, nullptr) == 0);
    FdSet huge{};
    huge.count = 1000; // a count over FD_SETSIZE reads 64 entries
    huge.sockets[63] = 0x200;
    CHECK(fdIsSet(0x200, &huge) == 1);
}

} // namespace sfcop::hle::winsock
