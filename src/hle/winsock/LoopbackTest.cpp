// The Winsock module on the real host Transport, over the loopback interface only.
//
// The first test case carries over, check for check and with the same expected values, the
// Winsock part of the Phase 1 spike's self-test (hle_selftest.c, since removed,
// test_winsock): a listener, a client that connects, select before and after, __WSAFDIsSet,
// accept, getpeername, a writable client, a half-close the peer sees. The spike checked a
// sockaddr's address as the guest's little-endian word 0x0100007F; here it is the host-order
// 0x7F000001 of WinsockSockAddr, the same 127.0.0.1.
#include "hle/winsock/Winsock.h"
#include "platform/Transport/Transport.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#if !defined(_WIN32)

namespace sfcop::hle::winsock {
namespace {

constexpr std::uint32_t kLoopback = 0x7F000001; // 127.0.0.1

bool skipTiming() {
    const char *value = std::getenv("SFCOP_SKIP_TIMING_TESTS");
    return value && std::strcmp(value, "1") == 0;
}

FdSet setOf(std::uint32_t s) {
    FdSet set{};
    set.count = 1;
    set.sockets[0] = s;
    return set;
}

} // namespace

TEST_CASE("loopback: the spike's Winsock checks pass on the module and the host transport") {
    auto transport = platform::makeTransport();
    Winsock w(*transport);
    WsaData wsa{};
    CHECK_MESSAGE(w.WsaStartup(0x101, &wsa) == 0, "WSAStartup");
    std::uint32_t ls = w.Socket(2, 1, 0);
    REQUIRE(ls != kInvalidSocket);
    SockAddrIn addr{2, 0, kLoopback}; // 127.0.0.1, any port
    CHECK_MESSAGE(w.Bind(ls, &addr, 16) == 0, "bind");
    CHECK_MESSAGE(w.Listen(ls, 4) == 0, "listen");
    std::int32_t len = 16;
    REQUIRE(w.GetSockName(ls, &addr, &len) == 0);
    FdSet set = setOf(ls);
    TimeVal tv{0, 0};
    CHECK_MESSAGE((w.Select(0, &set, nullptr, nullptr, &tv) == 0 && set.count == 0),
                  "select: nothing to accept yet");
    std::uint32_t cs = w.Socket(2, 1, 0);
    REQUIRE(cs != kInvalidSocket);
    CHECK_MESSAGE(w.Connect(cs, &addr, 16) == 0, "connect");
    set = setOf(ls);
    tv = TimeVal{1, 0};
    CHECK_MESSAGE(w.Select(0, &set, nullptr, nullptr, &tv) == 1,
                  "select: the listener is readable");
    CHECK_MESSAGE(fdIsSet(ls, &set) == 1, "__WSAFDIsSet: it is in the set");
    CHECK_MESSAGE(fdIsSet(cs, &set) == 0, "__WSAFDIsSet: another is not");
    len = 16;
    std::uint32_t as = w.Accept(ls, &addr, &len);
    CHECK_MESSAGE((as != kInvalidSocket && len == 16 && addr.family == 2 &&
                   addr.address == kLoopback),
                  "accept");
    SockAddrIn peer{};
    len = 16;
    CHECK_MESSAGE((w.GetPeerName(as, &peer, &len) == 0 && peer.address == kLoopback),
                  "getpeername: loopback");
    set = setOf(cs);
    tv = TimeVal{0, 0};
    CHECK_MESSAGE(w.Select(0, nullptr, &set, nullptr, &tv) == 1,
                  "select: the client is writable");
    CHECK_MESSAGE(w.Shutdown(cs, 1) == 0, "shutdown(SD_SEND)");
    set = setOf(as);
    tv = TimeVal{1, 0};
    CHECK_MESSAGE(w.Select(0, &set, nullptr, nullptr, &tv) == 1,
                  "select: the peer sees the end");
    CHECK(w.CloseSocket(as) == 0);
    CHECK(w.CloseSocket(cs) == 0);
    CHECK(w.CloseSocket(ls) == 0);
    CHECK(w.WsaCleanup() == 0);
}

TEST_CASE("loopback: a UDP datagram goes out with sendto and comes back with recvfrom") {
    auto transport = platform::makeTransport();
    Winsock w(*transport);
    WsaData wsa{};
    REQUIRE(w.WsaStartup(0x101, &wsa) == 0);
    std::uint32_t a = w.Socket(kAfInet, kSockDgram, kIpProtoUdp);
    std::uint32_t b = w.Socket(kAfInet, kSockDgram, kIpProtoUdp);
    SockAddrIn local{kAfInet, 0, kLoopback};
    REQUIRE(w.Bind(a, &local, 16) == 0);
    REQUIRE(w.Bind(b, &local, 16) == 0);
    SockAddrIn to{};
    std::int32_t len = 16;
    REQUIRE(w.GetSockName(b, &to, &len) == 0);
    SockAddrIn self{};
    REQUIRE(w.GetSockName(a, &self, &len) == 0);

    const char hello[] = "TestCaptain";
    CHECK(w.SendTo(a, hello, 11, 0, &to, 16) == 11);
    FdSet readable = setOf(b);
    TimeVal tv{2, 0};
    REQUIRE(w.Select(0, &readable, nullptr, nullptr, &tv) == 1);
    std::uint32_t waiting = 0;
    CHECK(w.IoctlSocket(b, kFionread, &waiting) == 0);
    CHECK(waiting == 11);
    char got[32] = {};
    SockAddrIn from{};
    len = 16;
    CHECK(w.RecvFrom(b, got, sizeof got, 0, &from, &len) == 11);
    CHECK(std::string(got) == "TestCaptain");
    CHECK(from.address == kLoopback);
    CHECK(from.port == self.port);

    std::uint32_t on = 1;
    REQUIRE(w.IoctlSocket(b, kFionbio, &on) == 0);
    CHECK(w.RecvFrom(b, got, sizeof got, 0, &from, &len) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEWouldBlock);
    CHECK(w.WsaCleanup() == 0);
}

TEST_CASE("loopback: a non-blocking connect ends writable and SO_ERROR reads 0") {
    auto transport = platform::makeTransport();
    Winsock w(*transport);
    WsaData wsa{};
    REQUIRE(w.WsaStartup(0x101, &wsa) == 0);
    std::uint32_t ls = w.Socket(kAfInet, kSockStream, kIpProtoTcp);
    SockAddrIn addr{kAfInet, 0, kLoopback};
    REQUIRE(w.Bind(ls, &addr, 16) == 0);
    REQUIRE(w.Listen(ls, 4) == 0);
    std::int32_t len = 16;
    REQUIRE(w.GetSockName(ls, &addr, &len) == 0);

    std::uint32_t cs = w.Socket(kAfInet, kSockStream, kIpProtoTcp);
    std::uint32_t on = 1;
    REQUIRE(w.IoctlSocket(cs, kFionbio, &on) == 0);
    std::int32_t started = w.Connect(cs, &addr, 16);
    REQUIRE((started == 0 || Winsock::WsaGetLastError() == kWsaEWouldBlock));
    FdSet writefds = setOf(cs);
    FdSet exceptfds = setOf(cs);
    TimeVal tv{2, 0};
    CHECK(w.Select(64, nullptr, &writefds, &exceptfds, &tv) == 1);
    CHECK(fdIsSet(cs, &writefds) == 1);
    std::uint32_t error = 0xFFFF;
    len = 4;
    CHECK(w.GetSockOpt(cs, kSolSocket, kSoError, &error, &len) == 0);
    CHECK(error == 0);
    CHECK(w.Connect(cs, &addr, 16) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEIsConn);
    CHECK(w.WsaCleanup() == 0);
}

// No portable way exists to see that a thread is parked inside recvfrom, so a delay stays between
// "about to call" and closesocket. If closesocket wins the race the receiver sees WSAENOTSOCK
// (it never parked) and the attempt is repeated on a fresh socket with a longer delay, up to four
// times. Guarded like the timed select tests.
TEST_CASE("loopback: closesocket from another thread ends a blocked recvfrom with WSAEINTR" *
          doctest::skip(skipTiming())) {
    auto transport = platform::makeTransport();
    Winsock w(*transport);
    WsaData wsa{};
    REQUIRE(w.WsaStartup(0x101, &wsa) == 0);
    std::int32_t result = 0;
    std::int32_t error = 0;
    for (int attempt = 0; attempt < 4 && error != kWsaEIntr; ++attempt) {
        std::uint32_t s = w.Socket(kAfInet, kSockDgram, 0);
        SockAddrIn local{kAfInet, 0, kLoopback};
        REQUIRE(w.Bind(s, &local, 16) == 0);
        std::atomic<bool> calling{false};
        std::thread receiver([&] {
            char buffer[8];
            calling.store(true);
            result = w.RecvFrom(s, buffer, sizeof buffer, 0, nullptr, nullptr);
            error = Winsock::WsaGetLastError();
        });
        while (!calling.load()) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50 << (attempt * 2)));
        REQUIRE(w.CloseSocket(s) == 0);
        receiver.join();
    }
    CHECK(result == kSocketError);
    CHECK(error == kWsaEIntr);
    CHECK(w.WsaCleanup() == 0);
}

// Tolerance as in TransportHostTest.cpp: no earlier than 195 ms, no later than 400 ms.
TEST_CASE("loopback: select with a 200 ms timeval returns 0 on time" *
          doctest::skip(skipTiming())) {
    auto transport = platform::makeTransport();
    Winsock w(*transport);
    WsaData wsa{};
    REQUIRE(w.WsaStartup(0x101, &wsa) == 0);
    std::uint32_t s = w.Socket(kAfInet, kSockDgram, 0);
    SockAddrIn local{kAfInet, 0, kLoopback};
    REQUIRE(w.Bind(s, &local, 16) == 0);
    FdSet set = setOf(s);
    TimeVal tv{0, 200000};
    auto start = std::chrono::steady_clock::now();
    std::int32_t ready = w.Select(0, &set, nullptr, nullptr, &tv);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();
    MESSAGE("select(200 ms timeval) returned after " << elapsed << " ms");
    CHECK(ready == 0);
    CHECK(set.count == 0);
    CHECK(elapsed >= 195);
    CHECK(elapsed <= 400);
    CHECK(w.WsaCleanup() == 0);
}

} // namespace sfcop::hle::winsock

#endif // !defined(_WIN32)
