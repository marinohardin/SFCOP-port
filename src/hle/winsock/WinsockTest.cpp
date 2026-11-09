// The Winsock module's entries as the game calls them, through the Transport mock. Addresses are
// from the RFC 5737 test ranges.
#include "hle/winsock/Winsock.h"
#include "platform/Transport/TransportMock.h"

#include <doctest/doctest.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace sfcop::hle::winsock {
namespace {

using platform::Endpoint;
using platform::Error;
using platform::MockDatagram;
using platform::SocketId;
using platform::TransportMock;

constexpr std::uint32_t kTestHost = 0xC0000201; // 192.0.2.1
constexpr std::uint32_t kTestPeer = 0xC6336402; // 198.51.100.2

SockAddrIn address(std::uint32_t host, std::uint16_t port) {
    SockAddrIn out{};
    out.family = kAfInet;
    out.port = port;
    out.address = host;
    return out;
}

// A started module on a mock, with the error slot cleared.
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
        mock.log.Clear();
        return s;
    }
};

} // namespace

TEST_CASE("WSAStartup(1.1) fills WSADATA and WSACleanup balances it") {
    TransportMock mock;
    Winsock winsock(mock);
    WsaData data{};
    CHECK(winsock.WsaStartup(0x0101, &data) == 0);
    CHECK(data.version == 0x0101);
    CHECK(data.highVersion == 0x0101);
    CHECK(std::string(data.systemStatus) == "Running");
    CHECK(data.maxUdpDatagram == 65467);
    CHECK(winsock.WsaStartup(0x0202, &data) == 0); // a later version gets 1.1
    CHECK(data.version == 0x0101);
    CHECK(winsock.WsaCleanup() == 0);
    CHECK(winsock.WsaCleanup() == 0);
    CHECK(winsock.WsaCleanup() == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaNotInitialised);
}

TEST_CASE("WSAStartup accepts 1.0, refuses version 0 and a missing WSADATA") {
    TransportMock mock;
    Winsock winsock(mock);
    WsaData data{};
    CHECK(winsock.WsaStartup(0x0000, &data) == kWsaVerNotSupported);
    CHECK(data.version == 0x0101);
    CHECK(winsock.WsaStartup(0x0001, &data) == 0); // 1.0 (major 1, minor 0), as the real stack
    CHECK(winsock.WsaCleanup() == 0);
    CHECK(winsock.WsaStartup(0x0101, nullptr) == kWsaEFault);
    Winsock::WsaSetLastError(0);
    CHECK(winsock.Socket(kAfInet, kSockStream, 0) == kInvalidSocket);
    CHECK(Winsock::WsaGetLastError() == kWsaNotInitialised);
    CHECK(mock.log.Count("Open") == 0);
}

TEST_CASE("WSAGetLastError returns what WSASetLastError left, per thread") {
    Winsock::WsaSetLastError(kWsaEWouldBlock);
    CHECK(Winsock::WsaGetLastError() == kWsaEWouldBlock);
    Winsock::WsaSetLastError(0);
    CHECK(Winsock::WsaGetLastError() == 0);
}

TEST_CASE("last error is per thread") {
    Winsock::WsaSetLastError(kWsaEWouldBlock);
    std::int32_t otherInitial = -1;
    std::int32_t otherAfterSet = -1;
    std::atomic<bool> set{false};
    std::atomic<bool> release{false};
    std::thread other([&] {
        otherInitial = Winsock::WsaGetLastError(); // a fresh thread starts at 0
        Winsock::WsaSetLastError(kWsaEIntr);
        otherAfterSet = Winsock::WsaGetLastError();
        set.store(true);
        while (!release.load()) {
            std::this_thread::yield();
        }
    });
    while (!set.load()) {
        std::this_thread::yield();
    }
    CHECK(Winsock::WsaGetLastError() == kWsaEWouldBlock); // the other thread's set did not leak
    release.store(true);
    other.join();
    CHECK(otherInitial == 0);
    CHECK(otherAfterSet == kWsaEIntr);
    CHECK(Winsock::WsaGetLastError() == kWsaEWouldBlock);
    Winsock::WsaSetLastError(0);
}

TEST_CASE("socket opens TCP and UDP sockets as distinct guest handles") {
    Fixture f;
    f.mock.openResult = SocketId{7};
    std::uint32_t tcp = f.winsock.Socket(kAfInet, kSockStream, kIpProtoTcp);
    f.mock.openResult = SocketId{8};
    std::uint32_t udp = f.winsock.Socket(kAfInet, kSockDgram, kIpProtoUdp);
    CHECK(tcp != kInvalidSocket);
    CHECK(udp != kInvalidSocket);
    CHECK(tcp != udp);
    CHECK(tcp % 4 == 0);
    CHECK(f.mock.log.Calls() == std::vector<std::string>{"Open(1)", "Open(0)"});
}

TEST_CASE("socket refuses other families, types and protocols") {
    Fixture f;
    CHECK(f.winsock.Socket(6, kSockDgram, 1000) == kInvalidSocket); // IPX
    CHECK(Winsock::WsaGetLastError() == kWsaEAfNoSupport);
    CHECK(f.winsock.Socket(kAfInet, 3, 0) == kInvalidSocket); // SOCK_RAW
    CHECK(Winsock::WsaGetLastError() == kWsaESocktNoSupport);
    CHECK(f.winsock.Socket(kAfInet, kSockStream, kIpProtoUdp) == kInvalidSocket);
    CHECK(Winsock::WsaGetLastError() == kWsaEProtoNoSupport);
    f.mock.openResult = Error::kOutOfResources;
    CHECK(f.winsock.Socket(kAfInet, kSockStream, 0) == kInvalidSocket);
    CHECK(Winsock::WsaGetLastError() == kWsaEMFile);
}

TEST_CASE("closesocket closes the transport socket and the handle is dead after") {
    Fixture f;
    std::uint32_t s = f.open(kSockStream, 7);
    CHECK(f.winsock.CloseSocket(s) == 0);
    CHECK(f.mock.log.Calls() == std::vector<std::string>{"Close(7)"});
    CHECK(f.winsock.CloseSocket(s) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaENotSock);
}

TEST_CASE("bind passes the address in host order") {
    Fixture f;
    std::uint32_t s = f.open(kSockDgram, 7);
    SockAddrIn local = address(kTestHost, 26900);
    CHECK(f.winsock.Bind(s, &local, 16) == 0);
    CHECK(f.mock.log.Calls() == std::vector<std::string>{"Bind(7, 3221225985, 26900)"});
}

TEST_CASE("bind's error paths") {
    Fixture f;
    std::uint32_t s = f.open(kSockDgram, 7);
    SockAddrIn local = address(kTestHost, 26900);
    CHECK(f.winsock.Bind(s, &local, 8) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEFault);
    SockAddrIn ipx = local;
    ipx.family = 6;
    CHECK(f.winsock.Bind(s, &ipx, 16) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEAfNoSupport);
    f.mock.bindResult = Error::kAddressInUse;
    CHECK(f.winsock.Bind(s, &local, 16) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEAddrInUse);
    f.mock.bindResult = Error::kInvalidArgument;
    CHECK(f.winsock.Bind(s, &local, 16) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEAddrNotAvail);
    CHECK(f.winsock.Bind(0x9999, &local, 16) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaENotSock);
}

TEST_CASE("connect: blocking success, and a non-blocking start reported as WSAEWOULDBLOCK") {
    Fixture f;
    std::uint32_t s = f.open(kSockStream, 7);
    SockAddrIn peer = address(kTestPeer, 6667);
    CHECK(f.winsock.Connect(s, &peer, 16) == 0);
    CHECK(f.mock.log.Calls() == std::vector<std::string>{"Connect(7, 3325256706, 6667)"});

    std::uint32_t n = f.open(kSockStream, 8);
    f.mock.connectResult = Error::kInProgress;
    CHECK(f.winsock.Connect(n, &peer, 16) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEWouldBlock);
    CHECK(f.winsock.Connect(n, &peer, 16) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEAlready);
    f.mock.connectResult = Error::kWrongState;
    CHECK(f.winsock.Connect(n, &peer, 16) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEIsConn);
}

TEST_CASE("connect's error paths") {
    Fixture f;
    std::uint32_t s = f.open(kSockStream, 7);
    SockAddrIn peer = address(kTestPeer, 6667);
    CHECK(f.winsock.Connect(s, nullptr, 16) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEFault);
    f.mock.connectResult = Error::kConnectionRefused;
    CHECK(f.winsock.Connect(s, &peer, 16) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEConnRefused);
    std::uint32_t value = 0;
    std::int32_t length = 4;
    CHECK(f.winsock.GetSockOpt(s, kSolSocket, kSoError, &value, &length) == 0);
    CHECK(value == static_cast<std::uint32_t>(kWsaEConnRefused));
    f.mock.connectResult = Error::kTimedOut;
    CHECK(f.winsock.Connect(s, &peer, 16) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaETimedOut);
    f.mock.connectResult = Error::kHostUnreachable;
    CHECK(f.winsock.Connect(s, &peer, 16) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEHostUnreach);
}

TEST_CASE("listen and accept hand out a new socket and the peer's address") {
    Fixture f;
    std::uint32_t s = f.open(kSockStream, 7);
    CHECK(f.winsock.Listen(s, 5) == 0);
    f.mock.acceptResult = platform::Accepted{SocketId{9}, Endpoint{kTestPeer, 50000}};
    SockAddrIn peer{};
    std::int32_t length = 16;
    std::uint32_t accepted = f.winsock.Accept(s, &peer, &length);
    CHECK(accepted != kInvalidSocket);
    CHECK(accepted != s);
    CHECK(length == 16);
    CHECK(peer.family == kAfInet);
    CHECK(peer.address == kTestPeer);
    CHECK(peer.port == 50000);
    CHECK(f.winsock.CloseSocket(accepted) == 0);
    CHECK(f.mock.log.Calls() ==
          std::vector<std::string>{"Listen(7, 5)", "Accept(7)", "Close(9)"});
}

TEST_CASE("listen and accept error paths") {
    Fixture f;
    std::uint32_t udp = f.open(kSockDgram, 7);
    CHECK(f.winsock.Listen(udp, 5) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEOpNotSupp);
    std::uint32_t tcp = f.open(kSockStream, 8);
    f.mock.listenResult = Error::kWrongState;
    CHECK(f.winsock.Listen(tcp, 5) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEInval);
    SockAddrIn peer{};
    std::int32_t small = 8;
    CHECK(f.winsock.Accept(tcp, &peer, &small) == kInvalidSocket);
    CHECK(Winsock::WsaGetLastError() == kWsaEFault);
    f.mock.acceptResult = Error::kWouldBlock;
    CHECK(f.winsock.Accept(tcp, nullptr, nullptr) == kInvalidSocket);
    CHECK(Winsock::WsaGetLastError() == kWsaEWouldBlock);
}

TEST_CASE("send and recv move the bytes the game gives and asks for") {
    Fixture f;
    std::uint32_t s = f.open(kSockStream, 7);
    const char message[] = "TestCaptain";
    CHECK(f.winsock.Send(s, message, 11, 0) == 11);
    REQUIRE(f.mock.sent.size() == 1);
    CHECK(f.mock.sent[0].bytes == std::vector<std::uint8_t>{'T', 'e', 's', 't', 'C', 'a', 'p',
                                                            't', 'a', 'i', 'n'});
    f.mock.incoming.push_back(MockDatagram{{0x01, 0x02, 0x03}, Endpoint{}});
    std::uint8_t buffer[8] = {};
    CHECK(f.winsock.Recv(s, buffer, sizeof buffer, 0) == 3);
    CHECK(buffer[0] == 0x01);
    CHECK(buffer[1] == 0x02);
    CHECK(buffer[2] == 0x03);
    CHECK(f.mock.log.Calls() == std::vector<std::string>{"Send(7, 11)", "Receive(7, 8)"});
}

TEST_CASE("send and recv error paths") {
    Fixture f;
    std::uint32_t s = f.open(kSockStream, 7);
    std::uint8_t buffer[8] = {};
    CHECK(f.winsock.Recv(s, buffer, sizeof buffer, 0) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEWouldBlock);
    CHECK(f.winsock.Recv(s, buffer, sizeof buffer, kMsgPeek) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEOpNotSupp);
    CHECK(f.winsock.Send(s, buffer, 4, kMsgOob) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEOpNotSupp);
    CHECK(f.winsock.Send(s, nullptr, 4, 0) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEFault);
    f.mock.sendError = Error::kConnectionReset;
    CHECK(f.winsock.Send(s, buffer, 4, 0) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEConnReset);
    f.mock.sendError = Error::kInvalidArgument;
    CHECK(f.winsock.Send(s, buffer, 4, 0) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEMsgSize);
    f.mock.sendError = Error::kNotConnected;
    CHECK(f.winsock.Send(s, buffer, 4, 0) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaENotConn);
}

TEST_CASE("sendto and recvfrom carry the peer's address") {
    Fixture f;
    std::uint32_t s = f.open(kSockDgram, 7);
    SockAddrIn to = address(kTestPeer, 26900);
    const std::uint8_t probe[] = {'P', 'I', 'N', 'G'};
    CHECK(f.winsock.SendTo(s, probe, 4, 0, &to, 16) == 4);
    REQUIRE(f.mock.sent.size() == 1);
    CHECK(f.mock.sent[0].peer == Endpoint{kTestPeer, 26900});

    f.mock.incoming.push_back(MockDatagram{{'P', 'O', 'N', 'G'}, Endpoint{kTestHost, 26901}});
    std::uint8_t buffer[16] = {};
    SockAddrIn from{};
    std::int32_t fromLength = 16;
    CHECK(f.winsock.RecvFrom(s, buffer, sizeof buffer, 0, &from, &fromLength) == 4);
    CHECK(from.family == kAfInet);
    CHECK(from.address == kTestHost);
    CHECK(from.port == 26901);
    CHECK(fromLength == 16);
    CHECK(buffer[0] == 'P');
    CHECK(buffer[3] == 'G');
    CHECK(f.mock.log.Calls() == std::vector<std::string>{"SendTo(7, 4, 3325256706, 26900)",
                                                         "ReceiveFrom(7, 16)"});
}

TEST_CASE("sendto and recvfrom error paths") {
    Fixture f;
    std::uint32_t s = f.open(kSockDgram, 7);
    SockAddrIn to = address(0xFFFFFFFF, 26900);
    std::uint8_t buffer[4] = {};
    f.mock.sendError = Error::kAccessDenied;
    CHECK(f.winsock.SendTo(s, buffer, 4, 0, &to, 16) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEAcces);
    CHECK(f.winsock.SendTo(s, buffer, 4, 0, &to, 4) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEFault);
    SockAddrIn from{};
    std::int32_t small = 4;
    CHECK(f.winsock.RecvFrom(s, buffer, 4, 0, &from, &small) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEFault);
    CHECK(f.winsock.RecvFrom(s, buffer, 4, 0, nullptr, nullptr) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEWouldBlock);
}

TEST_CASE("sendto with no address sends on the connected peer") {
    Fixture f;
    std::uint32_t s = f.open(kSockDgram, 7);
    std::uint8_t buffer[2] = {1, 2};
    CHECK(f.winsock.SendTo(s, buffer, 2, 0, nullptr, 0) == 2);
    CHECK(f.mock.log.Calls() == std::vector<std::string>{"Send(7, 2)"});
}

TEST_CASE("shutdown maps SD_RECEIVE, SD_SEND and SD_BOTH") {
    Fixture f;
    std::uint32_t s = f.open(kSockStream, 7);
    CHECK(f.winsock.Shutdown(s, kSdReceive) == 0);
    CHECK(f.winsock.Shutdown(s, kSdSend) == 0);
    CHECK(f.winsock.Shutdown(s, kSdBoth) == 0);
    CHECK(f.mock.log.Calls() ==
          std::vector<std::string>{"Shutdown(7, 0)", "Shutdown(7, 1)", "Shutdown(7, 2)"});
    CHECK(f.winsock.Shutdown(s, 3) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEInval);
    f.mock.shutdownResult = Error::kNotConnected;
    CHECK(f.winsock.Shutdown(s, kSdBoth) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaENotConn);
}

TEST_CASE("getpeername and getsockname write a sockaddr_in and its length") {
    Fixture f;
    std::uint32_t s = f.open(kSockStream, 7);
    f.mock.peerNameResult = Endpoint{kTestPeer, 6667};
    f.mock.localNameResult = Endpoint{kTestHost, 50001};
    SockAddrIn name{};
    std::int32_t length = 16;
    CHECK(f.winsock.GetPeerName(s, &name, &length) == 0);
    CHECK(name.address == kTestPeer);
    CHECK(name.port == 6667);
    CHECK(length == 16);
    CHECK(f.winsock.GetSockName(s, &name, &length) == 0);
    CHECK(name.address == kTestHost);
    CHECK(name.port == 50001);
}

TEST_CASE("getpeername and getsockname error paths") {
    Fixture f;
    std::uint32_t s = f.open(kSockStream, 7);
    SockAddrIn name{};
    std::int32_t length = 16;
    f.mock.peerNameResult = Error::kNotConnected;
    CHECK(f.winsock.GetPeerName(s, &name, &length) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaENotConn);
    f.mock.localNameResult = Error::kWrongState;
    CHECK(f.winsock.GetSockName(s, &name, &length) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEInval);
    std::int32_t small = 15;
    CHECK(f.winsock.GetSockName(s, &name, &small) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEFault);
}

TEST_CASE("ioctlsocket: FIONBIO sets non-blocking and FIONREAD reads what is waiting") {
    Fixture f;
    std::uint32_t s = f.open(kSockStream, 7);
    std::uint32_t on = 1;
    CHECK(f.winsock.IoctlSocket(s, kFionbio, &on) == 0);
    f.mock.getOptionResult = std::int32_t{12};
    std::uint32_t waiting = 0;
    CHECK(f.winsock.IoctlSocket(s, kFionread, &waiting) == 0);
    CHECK(waiting == 12);
    std::uint32_t mark = 0;
    CHECK(f.winsock.IoctlSocket(s, kSiocatmark, &mark) == 0);
    CHECK(mark == 1);
    CHECK(f.mock.log.Calls() ==
          std::vector<std::string>{"SetOption(7, 0, 1)", "GetOption(7, 8)"});
    CHECK(f.winsock.IoctlSocket(s, 0x12345678, &on) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEInval);
    CHECK(f.winsock.IoctlSocket(s, kFionbio, nullptr) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEFault);
}

TEST_CASE("setsockopt: the options the game sets reach the transport") {
    Fixture f;
    std::uint32_t tcp = f.open(kSockStream, 7);
    std::uint32_t lingerOff = 0x00000000; // l_onoff 0
    CHECK(f.winsock.SetSockOpt(tcp, kSolSocket, kSoLinger, &lingerOff, 4) == 0);
    std::uint32_t lingerFive = 0x00050001; // l_onoff 1, l_linger 5
    CHECK(f.winsock.SetSockOpt(tcp, kSolSocket, kSoLinger, &lingerFive, 4) == 0);
    std::uint32_t one = 1;
    CHECK(f.winsock.SetSockOpt(tcp, kSolSocket, kSoReuseAddr, &one, 4) == 0);
    CHECK(f.winsock.SetSockOpt(tcp, kIpProtoTcp, kTcpNoDelay, &one, 4) == 0);
    CHECK(f.winsock.SetSockOpt(tcp, kSolSocket, kSoKeepAlive, &one, 4) == 0);
    std::uint32_t size = 16384;
    CHECK(f.winsock.SetSockOpt(tcp, kSolSocket, kSoRcvBuf, &size, 4) == 0);
    CHECK(f.winsock.SetSockOpt(tcp, kSolSocket, kSoSndBuf, &size, 4) == 0);
    CHECK(f.winsock.SetSockOpt(tcp, kSolSocket, kSoDontLinger, &one, 4) == 0);
    std::uint32_t udp = f.open(kSockDgram, 8);
    CHECK(f.winsock.SetSockOpt(udp, kSolSocket, kSoBroadcast, &one, 4) == 0);
    CHECK(f.mock.log.Calls() == std::vector<std::string>{"SetOption(8, 1, 1)"});
}

TEST_CASE("setsockopt: the transport receives each option's value") {
    Fixture f;
    std::uint32_t tcp = f.open(kSockStream, 7);
    std::uint32_t lingerFive = 0x00050001;
    std::uint32_t lingerOff = 0;
    std::uint32_t one = 1;
    CHECK(f.winsock.SetSockOpt(tcp, kSolSocket, kSoLinger, &lingerOff, 4) == 0);
    CHECK(f.winsock.SetSockOpt(tcp, kSolSocket, kSoLinger, &lingerFive, 4) == 0);
    CHECK(f.winsock.SetSockOpt(tcp, kIpProtoTcp, kTcpNoDelay, &one, 4) == 0);
    CHECK(f.mock.log.Calls() == std::vector<std::string>{"SetOption(7, 7, -1)",
                                                         "SetOption(7, 7, 5)",
                                                         "SetOption(7, 3, 1)"});
}

TEST_CASE("setsockopt error paths") {
    Fixture f;
    std::uint32_t udp = f.open(kSockDgram, 7);
    std::uint32_t one = 1;
    CHECK(f.winsock.SetSockOpt(udp, kSolSocket, kSoLinger, &one, 4) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaENoProtoOpt);
    CHECK(f.winsock.SetSockOpt(udp, kIpProtoTcp, kTcpNoDelay, &one, 4) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaENoProtoOpt);
    CHECK(f.winsock.SetSockOpt(udp, kSolSocket, 0x0010, &one, 4) == kSocketError); // SO_DONTROUTE
    CHECK(Winsock::WsaGetLastError() == kWsaENoProtoOpt);
    CHECK(f.winsock.SetSockOpt(udp, 0x1234, 1, &one, 4) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEInval);
    CHECK(f.winsock.SetSockOpt(udp, kSolSocket, kSoBroadcast, &one, 2) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEFault);
    CHECK(f.winsock.SetSockOpt(udp, kSolSocket, kSoBroadcast, nullptr, 4) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEFault);
    CHECK(f.mock.log.Count("SetOption") == 0);
}

TEST_CASE("getsockopt: SO_ERROR, SO_TYPE, SO_LINGER and a plain option") {
    Fixture f;
    std::uint32_t tcp = f.open(kSockStream, 7);
    std::uint32_t value = 0xAAAAAAAA;
    std::int32_t length = 4;
    CHECK(f.winsock.GetSockOpt(tcp, kSolSocket, kSoError, &value, &length) == 0);
    CHECK(value == 0);
    CHECK(length == 4);
    CHECK(f.winsock.GetSockOpt(tcp, kSolSocket, kSoType, &value, &length) == 0);
    CHECK(value == 1);
    f.mock.getOptionResult = std::int32_t{-1};
    CHECK(f.winsock.GetSockOpt(tcp, kSolSocket, kSoLinger, &value, &length) == 0);
    CHECK(value == 0x00000000);
    CHECK(f.winsock.GetSockOpt(tcp, kSolSocket, kSoDontLinger, &value, &length) == 0);
    CHECK(value == 1);
    f.mock.getOptionResult = std::int32_t{5};
    CHECK(f.winsock.GetSockOpt(tcp, kSolSocket, kSoLinger, &value, &length) == 0);
    CHECK(value == 0x00050001);
    f.mock.getOptionResult = std::int32_t{8192};
    CHECK(f.winsock.GetSockOpt(tcp, kSolSocket, kSoRcvBuf, &value, &length) == 0);
    CHECK(value == 8192);
    std::int32_t small = 2;
    CHECK(f.winsock.GetSockOpt(tcp, kSolSocket, kSoType, &value, &small) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEFault);
    f.mock.getOptionResult = Error::kInvalidHandle;
    CHECK(f.winsock.GetSockOpt(tcp, kSolSocket, kSoRcvBuf, &value, &length) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaENotSock);
}

TEST_CASE("gethostbyname resolves through the transport; NULL is this machine") {
    Fixture f;
    f.mock.resolveResult = std::vector<std::uint32_t>{kTestHost, kTestPeer};
    std::optional<HostEntry> found = f.winsock.GetHostByName("server.test");
    REQUIRE(found);
    CHECK(found->name == "server.test");
    CHECK(found->addresses == std::vector<std::uint32_t>{kTestHost, kTestPeer});
    std::optional<HostEntry> self = f.winsock.GetHostByName(nullptr);
    REQUIRE(self);
    CHECK(self->name == "testhost");
    CHECK(f.mock.log.Calls() == std::vector<std::string>{"Resolve(\"server.test\")", "HostName()",
                                                         "Resolve(\"testhost\")"});
}

TEST_CASE("gethostbyname resolves an aliased name as its target and keeps the name asked") {
    Fixture f;
    f.mock.resolveResult = std::vector<std::uint32_t>{kTestHost};
    f.winsock.SetHostAliases({{"Directory.Example.Test", "127.0.0.1"}});
    std::optional<HostEntry> found = f.winsock.GetHostByName("directory.example.TEST");
    REQUIRE(found);
    CHECK(found->name == "directory.example.TEST");
    CHECK(found->addresses == std::vector<std::uint32_t>{kTestHost});
    CHECK(f.winsock.GetHostByName("other.test"));
    f.winsock.SetHostAliases({});
    CHECK(f.winsock.GetHostByName("directory.example.test"));
    CHECK(f.mock.log.Calls() == std::vector<std::string>{"Resolve(\"127.0.0.1\")",
                                                         "Resolve(\"other.test\")",
                                                         "Resolve(\"directory.example.test\")"});
}

TEST_CASE("an aliased name whose target does not resolve fails as an unknown host") {
    Fixture f;
    f.winsock.SetHostAliases({{"directory.example.test", "standin.invalid"}});
    f.mock.resolveResult = Error::kNotFound;
    CHECK_FALSE(f.winsock.GetHostByName("directory.example.test"));
    CHECK(Winsock::WsaGetLastError() == kWsaHostNotFound);
    CHECK(f.mock.log.Calls() == std::vector<std::string>{"Resolve(\"standin.invalid\")"});
}

TEST_CASE("gethostbyname error paths give the resolver errors") {
    Fixture f;
    f.mock.resolveResult = Error::kNotFound;
    CHECK_FALSE(f.winsock.GetHostByName("nowhere.invalid"));
    CHECK(Winsock::WsaGetLastError() == kWsaHostNotFound);
    f.mock.resolveResult = Error::kTimedOut;
    CHECK_FALSE(f.winsock.GetHostByName("slow.invalid"));
    CHECK(Winsock::WsaGetLastError() == kWsaTryAgain);
    f.mock.resolveResult = Error::kIoError;
    CHECK_FALSE(f.winsock.GetHostByName("broken.invalid"));
    CHECK(Winsock::WsaGetLastError() == kWsaNoRecovery);
}

TEST_CASE("gethostname copies the name and refuses a short buffer") {
    Fixture f;
    char name[32];
    CHECK(f.winsock.GetHostName(name, sizeof name) == 0);
    CHECK(std::string(name) == "testhost");
    char small[8];
    CHECK(f.winsock.GetHostName(small, sizeof small) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaEFault);
    f.mock.hostNameResult = Error::kIoError;
    CHECK(f.winsock.GetHostName(name, sizeof name) == kSocketError);
    CHECK(Winsock::WsaGetLastError() == kWsaENetDown);
}

TEST_CASE("WSACleanup closes the sockets the game left open") {
    Fixture f;
    f.open(kSockStream, 7);
    f.open(kSockDgram, 8);
    CHECK(f.winsock.WsaCleanup() == 0);
    CHECK(f.mock.log.Count("Close") == 2);
}

TEST_CASE("every Transport error has a Winsock error") {
    CHECK(wsaErrorOf(Error::kNone) == 0);
    CHECK(wsaErrorOf(Error::kUnsupported) == 10045);
    CHECK(wsaErrorOf(Error::kInvalidArgument) == 10022);
    CHECK(wsaErrorOf(Error::kInvalidHandle) == 10038);
    CHECK(wsaErrorOf(Error::kWrongState) == 10022);
    CHECK(wsaErrorOf(Error::kNotFound) == 11001);
    CHECK(wsaErrorOf(Error::kAlreadyExists) == 10048);
    CHECK(wsaErrorOf(Error::kAccessDenied) == 10013);
    CHECK(wsaErrorOf(Error::kNotOwner) == 10022);
    CHECK(wsaErrorOf(Error::kOutOfResources) == 10055);
    CHECK(wsaErrorOf(Error::kIoError) == 10050);
    CHECK(wsaErrorOf(Error::kWouldBlock) == 10035);
    CHECK(wsaErrorOf(Error::kInProgress) == 10035);
    CHECK(wsaErrorOf(Error::kTimedOut) == 10060);
    CHECK(wsaErrorOf(Error::kConnectionRefused) == 10061);
    CHECK(wsaErrorOf(Error::kConnectionReset) == 10054);
    CHECK(wsaErrorOf(Error::kNotConnected) == 10057);
    CHECK(wsaErrorOf(Error::kAddressInUse) == 10048);
    CHECK(wsaErrorOf(Error::kHostUnreachable) == 10065);
}

TEST_CASE("the C entry points work on the installed instance") {
    installGuestWinsock(std::make_unique<TransportMock>());
    WinsockData data{};
    CHECK(sfcop_winsock_WSAStartup(0x0101, &data) == 0);
    std::uint32_t s = sfcop_winsock_socket(kAfInet, kSockDgram, 0);
    CHECK(s != kInvalidSocket);
    WinsockSockAddr local = address(kTestHost, 26900);
    CHECK(sfcop_winsock_bind(s, &local, 16) == 0);
    WinsockHostEntry entry{};
    CHECK(sfcop_winsock_gethostbyname("server.test", &entry) == 0); // the mock has no address
    CHECK(sfcop_winsock_WSAGetLastError() == kWsaNoData);
    sfcop_winsock_WSASetLastError(0);
    CHECK(sfcop_winsock_WSAGetLastError() == 0);
    char text[16];
    sfcop_winsock_inet_ntoa(0x0102000A, text);
    CHECK(std::string(text) == "10.0.2.1");
    CHECK(sfcop_winsock_closesocket(s) == 0);
    CHECK(sfcop_winsock_WSACleanup() == 0);
    installGuestWinsock(platform::makeTransportStub());
}

} // namespace sfcop::hle::winsock
