// The host sockets backend, on the loopback interface only: no test reaches another host.
#include "platform/Transport/Transport.h"
#include "platform/Transport/TransportHost.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#if !defined(_WIN32)

namespace sfcop::platform {
namespace {

constexpr std::uint32_t kLoopback = 0x7F000001; // 127.0.0.1

bool skipTiming() {
    const char *value = std::getenv("SFCOP_SKIP_TIMING_TESTS");
    return value && std::strcmp(value, "1") == 0;
}

// A socket bound to 127.0.0.1 on a free port, and that port.
struct Bound {
    SocketId socket;
    Endpoint local;
};

Bound bindLoopback(Transport &transport, SocketKind kind) {
    Result<SocketId> socket = transport.Open(kind);
    REQUIRE(socket.Ok());
    REQUIRE(transport.Bind(socket.Value(), Endpoint{kLoopback, 0}).Ok());
    Result<Endpoint> local = transport.LocalName(socket.Value());
    REQUIRE(local.Ok());
    CHECK(local.Value().address == kLoopback);
    CHECK(local.Value().port != 0);
    return Bound{socket.Value(), local.Value()};
}

// A connected TCP pair: the listener, the client and the server's end of the connection.
struct TcpPair {
    Bound listener;
    SocketId client;
    SocketId server;
};

TcpPair connectLoopback(Transport &transport) {
    TcpPair pair{bindLoopback(transport, SocketKind::kTcp), {}, {}};
    REQUIRE(transport.Listen(pair.listener.socket, 4).Ok());
    Result<SocketId> client = transport.Open(SocketKind::kTcp);
    REQUIRE(client.Ok());
    pair.client = client.Value();
    REQUIRE(transport.Connect(pair.client, pair.listener.local).Ok());
    Result<Accepted> accepted = transport.Accept(pair.listener.socket);
    REQUIRE(accepted.Ok());
    pair.server = accepted.Value().socket;
    return pair;
}

// Waits up to two seconds for a socket to become readable (loopback delivery is not instant).
void waitReadable(Transport &transport, SocketId socket) {
    SelectSet sets;
    sets.readable.push_back(socket);
    Result<std::size_t> ready = transport.Select(sets, 2000u);
    REQUIRE(ready.Ok());
    REQUIRE(ready.Value() == 1);
}

void closeAll(Transport &transport, std::initializer_list<SocketId> sockets) {
    for (SocketId socket : sockets) {
        CHECK(transport.Close(socket).Ok());
    }
}

} // namespace

TEST_CASE("host transport: a UDP pair exchanges datagrams and names the sender") {
    auto transport = makeTransport();
    Bound a = bindLoopback(*transport, SocketKind::kUdp);
    Bound b = bindLoopback(*transport, SocketKind::kUdp);

    const std::uint8_t hello[] = {0x48, 0x45, 0x4C, 0x4C, 0x4F, 0x00, 0xFF};
    Result<std::size_t> sent = transport->SendTo(a.socket, hello, sizeof hello, b.local);
    REQUIRE(sent.Ok());
    CHECK(sent.Value() == 7);

    std::uint8_t got[16] = {};
    Result<Received> received = transport->ReceiveFrom(b.socket, got, sizeof got);
    REQUIRE(received.Ok());
    CHECK(received.Value().bytes == 7);
    CHECK(received.Value().from == a.local);
    const std::uint8_t expected[16] = {0x48, 0x45, 0x4C, 0x4C, 0x4F, 0x00, 0xFF};
    CHECK(std::memcmp(got, expected, sizeof expected) == 0);

    // Connected: Send() and Receive() talk to the one peer.
    REQUIRE(transport->Connect(b.socket, a.local).Ok());
    REQUIRE(transport->Connect(a.socket, b.local).Ok());
    const std::uint8_t reply[] = {0x01, 0x02, 0x03};
    REQUIRE(transport->Send(b.socket, reply, sizeof reply).Ok());
    std::uint8_t back[3] = {};
    Result<std::size_t> n = transport->Receive(a.socket, back, sizeof back);
    REQUIRE(n.Ok());
    CHECK(n.Value() == 3);
    CHECK(back[0] == 0x01);
    CHECK(back[1] == 0x02);
    CHECK(back[2] == 0x03);
    Result<Endpoint> peer = transport->PeerName(a.socket);
    REQUIRE(peer.Ok());
    CHECK(peer.Value() == b.local);
    closeAll(*transport, {a.socket, b.socket});
}

TEST_CASE("host transport: a datagram longer than the buffer is truncated and the rest lost") {
    auto transport = makeTransport();
    Bound a = bindLoopback(*transport, SocketKind::kUdp);
    Bound b = bindLoopback(*transport, SocketKind::kUdp);
    const std::uint8_t five[] = {10, 20, 30, 40, 50};
    const std::uint8_t one[] = {99};
    REQUIRE(transport->SendTo(a.socket, five, sizeof five, b.local).Ok());
    REQUIRE(transport->SendTo(a.socket, one, sizeof one, b.local).Ok());
    waitReadable(*transport, b.socket);

    Result<std::int32_t> readable = transport->GetOption(b.socket, SocketOption::kBytesReadable);
    REQUIRE(readable.Ok());
    CHECK(readable.Value() == 5);

    std::uint8_t got[2] = {};
    Result<Received> first = transport->ReceiveFrom(b.socket, got, sizeof got);
    REQUIRE(first.Ok());
    CHECK(first.Value().bytes == 2);
    CHECK(got[0] == 10);
    CHECK(got[1] == 20);
    Result<Received> second = transport->ReceiveFrom(b.socket, got, sizeof got);
    REQUIRE(second.Ok());
    CHECK(second.Value().bytes == 1);
    CHECK(got[0] == 99);
    closeAll(*transport, {a.socket, b.socket});
}

TEST_CASE("host transport: a TCP pair exchanges a stream both ways") {
    auto transport = makeTransport();
    TcpPair pair = connectLoopback(*transport);

    Result<Endpoint> clientLocal = transport->LocalName(pair.client);
    Result<Endpoint> serverPeer = transport->PeerName(pair.server);
    REQUIRE(clientLocal.Ok());
    REQUIRE(serverPeer.Ok());
    CHECK(serverPeer.Value() == clientLocal.Value());
    Result<Endpoint> clientPeer = transport->PeerName(pair.client);
    REQUIRE(clientPeer.Ok());
    CHECK(clientPeer.Value() == pair.listener.local);

    const std::uint8_t request[] = {0xDE, 0xAD, 0xBE, 0xEF};
    Result<std::size_t> sent = transport->Send(pair.client, request, sizeof request);
    REQUIRE(sent.Ok());
    CHECK(sent.Value() == 4);
    std::uint8_t got[4] = {};
    std::size_t total = 0;
    while (total < sizeof got) {
        Result<std::size_t> n = transport->Receive(pair.server, got + total, sizeof got - total);
        REQUIRE(n.Ok());
        REQUIRE(n.Value() > 0);
        total += n.Value();
    }
    CHECK(got[0] == 0xDE);
    CHECK(got[1] == 0xAD);
    CHECK(got[2] == 0xBE);
    CHECK(got[3] == 0xEF);

    const std::uint8_t response[] = {0x00, 0x7F};
    REQUIRE(transport->Send(pair.server, response, sizeof response).Ok());
    std::uint8_t back[2] = {};
    total = 0;
    while (total < sizeof back) {
        Result<std::size_t> n = transport->Receive(pair.client, back + total, sizeof back - total);
        REQUIRE(n.Ok());
        REQUIRE(n.Value() > 0);
        total += n.Value();
    }
    CHECK(back[0] == 0x00);
    CHECK(back[1] == 0x7F);
    closeAll(*transport, {pair.server, pair.client, pair.listener.socket});
}

TEST_CASE("host transport: non-blocking calls return kWouldBlock instead of waiting") {
    auto transport = makeTransport();
    Bound udp = bindLoopback(*transport, SocketKind::kUdp);
    REQUIRE(transport->SetOption(udp.socket, SocketOption::kNonBlocking, 1).Ok());
    std::uint8_t buffer[8];
    Result<Received> nothing = transport->ReceiveFrom(udp.socket, buffer, sizeof buffer);
    CHECK(nothing.Code() == Error::kWouldBlock);

    Bound listener = bindLoopback(*transport, SocketKind::kTcp);
    REQUIRE(transport->Listen(listener.socket, 4).Ok());
    REQUIRE(transport->SetOption(listener.socket, SocketOption::kNonBlocking, 1).Ok());
    CHECK(transport->Accept(listener.socket).Code() == Error::kWouldBlock);
    closeAll(*transport, {udp.socket, listener.socket});
}

TEST_CASE("host transport: a non-blocking connect finishes as writable and is accepted") {
    auto transport = makeTransport();
    Bound listener = bindLoopback(*transport, SocketKind::kTcp);
    REQUIRE(transport->Listen(listener.socket, 4).Ok());
    Result<SocketId> client = transport->Open(SocketKind::kTcp);
    REQUIRE(client.Ok());
    REQUIRE(transport->SetOption(client.Value(), SocketOption::kNonBlocking, 1).Ok());

    Status connect = transport->Connect(client.Value(), listener.local);
    REQUIRE((connect.Ok() || connect.Code() == Error::kInProgress));
    SelectSet sets;
    sets.writable.push_back(client.Value());
    sets.errored.push_back(client.Value());
    Result<std::size_t> ready = transport->Select(sets, 2000u);
    REQUIRE(ready.Ok());
    CHECK(ready.Value() == 1);
    REQUIRE(sets.writable.size() == 1);
    CHECK(sets.writable[0] == client.Value());
    CHECK(sets.errored.empty());

    // The pending connection: the listener is readable, then Accept() takes it.
    SelectSet incoming;
    incoming.readable.push_back(listener.socket);
    Result<std::size_t> waiting = transport->Select(incoming, 2000u);
    REQUIRE(waiting.Ok());
    CHECK(waiting.Value() == 1);
    Result<Accepted> accepted = transport->Accept(listener.socket);
    REQUIRE(accepted.Ok());
    Result<Endpoint> clientLocal = transport->LocalName(client.Value());
    REQUIRE(clientLocal.Ok());
    CHECK(accepted.Value().peer == clientLocal.Value());
    CHECK(transport->Connect(client.Value(), listener.local).Code() == Error::kWrongState);
    closeAll(*transport, {accepted.Value().socket, client.Value(), listener.socket});
}

TEST_CASE("host transport: a refused connect fails, and shows as errored when non-blocking") {
    auto transport = makeTransport();
    // A port that was free a moment ago and has no listener now.
    Bound probe = bindLoopback(*transport, SocketKind::kTcp);
    Endpoint nobody = probe.local;
    REQUIRE(transport->Close(probe.socket).Ok());

    Result<SocketId> blocking = transport->Open(SocketKind::kTcp);
    REQUIRE(blocking.Ok());
    CHECK(transport->Connect(blocking.Value(), nobody).Code() == Error::kConnectionRefused);

    Result<SocketId> client = transport->Open(SocketKind::kTcp);
    REQUIRE(client.Ok());
    REQUIRE(transport->SetOption(client.Value(), SocketOption::kNonBlocking, 1).Ok());
    Status connect = transport->Connect(client.Value(), nobody);
    if (connect.Code() == Error::kInProgress) {
        SelectSet sets;
        sets.writable.push_back(client.Value());
        sets.errored.push_back(client.Value());
        Result<std::size_t> ready = transport->Select(sets, 2000u);
        REQUIRE(ready.Ok());
        CHECK(ready.Value() == 1);
        CHECK(sets.writable.empty());
        REQUIRE(sets.errored.size() == 1);
        CHECK(sets.errored[0] == client.Value());
    } else {
        CHECK(connect.Code() == Error::kConnectionRefused);
    }
    closeAll(*transport, {blocking.Value(), client.Value()});
}

TEST_CASE("host transport: Shutdown(kSend) half-closes; the other direction still flows") {
    auto transport = makeTransport();
    TcpPair pair = connectLoopback(*transport);
    REQUIRE(transport->Shutdown(pair.client, ShutdownHow::kSend).Ok());

    SelectSet sets;
    sets.readable.push_back(pair.server);
    Result<std::size_t> ready = transport->Select(sets, 2000u);
    REQUIRE(ready.Ok());
    CHECK(ready.Value() == 1);
    std::uint8_t buffer[4];
    Result<std::size_t> end = transport->Receive(pair.server, buffer, sizeof buffer);
    REQUIRE(end.Ok());
    CHECK(end.Value() == 0);

    const std::uint8_t late[] = {0x42};
    REQUIRE(transport->Send(pair.server, late, sizeof late).Ok());
    Result<std::size_t> got = transport->Receive(pair.client, buffer, sizeof buffer);
    REQUIRE(got.Ok());
    CHECK(got.Value() == 1);
    CHECK(buffer[0] == 0x42);
    closeAll(*transport, {pair.server, pair.client, pair.listener.socket});
}

TEST_CASE("host transport: Select with a zero timeout polls, and reports a waiting datagram") {
    auto transport = makeTransport();
    Bound a = bindLoopback(*transport, SocketKind::kUdp);
    Bound b = bindLoopback(*transport, SocketKind::kUdp);
    SelectSet empty;
    empty.readable.push_back(b.socket);
    Result<std::size_t> none = transport->Select(empty, 0u);
    REQUIRE(none.Ok());
    CHECK(none.Value() == 0);
    CHECK(empty.readable.empty());

    const std::uint8_t ping[] = {'P'};
    REQUIRE(transport->SendTo(a.socket, ping, sizeof ping, b.local).Ok());
    waitReadable(*transport, b.socket);
    SelectSet sets;
    sets.readable.push_back(a.socket);
    sets.readable.push_back(b.socket);
    sets.writable.push_back(a.socket);
    Result<std::size_t> ready = transport->Select(sets, std::nullopt);
    REQUIRE(ready.Ok());
    CHECK(ready.Value() == 2);
    REQUIRE(sets.readable.size() == 1);
    CHECK(sets.readable[0] == b.socket);
    REQUIRE(sets.writable.size() == 1);
    CHECK(sets.writable[0] == a.socket);
    closeAll(*transport, {a.socket, b.socket});
}

// Tolerance: the call returns no earlier than 195 ms (poll counts whole milliseconds) and no later
// than 400 ms, which leaves room for a loaded machine.
TEST_CASE("host transport: a 200 ms Select timeout returns on time" *
          doctest::skip(skipTiming())) {
    auto transport = makeTransport();
    Bound idle = bindLoopback(*transport, SocketKind::kUdp);
    SelectSet sets;
    sets.readable.push_back(idle.socket);
    auto start = std::chrono::steady_clock::now();
    Result<std::size_t> ready = transport->Select(sets, 200u);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();
    MESSAGE("select(200 ms) returned after " << elapsed << " ms");
    REQUIRE(ready.Ok());
    CHECK(ready.Value() == 0);
    CHECK(sets.readable.empty());
    CHECK(elapsed >= 195);
    CHECK(elapsed <= 400);
    CHECK(transport->Close(idle.socket).Ok());
}

// There is no portable way to observe that a thread is parked inside recvfrom, so this case keeps
// a delay between "about to call" and Close. It cannot flake into a false failure: when Close wins
// the race the receiver sees kInvalidHandle (it never parked), and the attempt is repeated on a
// fresh socket with a longer delay, up to four times. It is guarded like the timed select tests.
TEST_CASE("host transport: Close from another thread wakes a blocked Receive" *
          doctest::skip(skipTiming())) {
    auto transport = makeTransport();
    Error result = Error::kInvalidHandle;
    for (int attempt = 0; attempt < 4 && result != Error::kNotConnected; ++attempt) {
        Bound udp = bindLoopback(*transport, SocketKind::kUdp);
        std::atomic<bool> calling{false};
        result = Error::kNone;
        std::thread receiver([&] {
            std::uint8_t buffer[8];
            calling.store(true);
            Result<Received> got = transport->ReceiveFrom(udp.socket, buffer, sizeof buffer);
            result = got.Ok() ? Error::kNone : got.Code();
        });
        while (!calling.load()) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50 << (attempt * 2)));
        REQUIRE(transport->Close(udp.socket).Ok());
        receiver.join();
        CHECK(result != Error::kNone);
    }
    CHECK(result == Error::kNotConnected);
}

TEST_CASE("host transport: options read back what was set") {
    auto transport = makeTransport();
    Result<SocketId> tcp = transport->Open(SocketKind::kTcp);
    Result<SocketId> udp = transport->Open(SocketKind::kUdp);
    REQUIRE(tcp.Ok());
    REQUIRE(udp.Ok());
    SocketId t = tcp.Value();
    SocketId u = udp.Value();

    CHECK(transport->GetOption(t, SocketOption::kNonBlocking).Value() == 0);
    REQUIRE(transport->SetOption(t, SocketOption::kNonBlocking, 1).Ok());
    CHECK(transport->GetOption(t, SocketOption::kNonBlocking).Value() == 1);
    REQUIRE(transport->SetOption(u, SocketOption::kBroadcast, 1).Ok());
    CHECK(transport->GetOption(u, SocketOption::kBroadcast).Value() == 1);
    REQUIRE(transport->SetOption(u, SocketOption::kReuseAddress, 1).Ok());
    CHECK(transport->GetOption(u, SocketOption::kReuseAddress).Value() == 1);
    REQUIRE(transport->SetOption(t, SocketOption::kNoDelay, 1).Ok());
    CHECK(transport->GetOption(t, SocketOption::kNoDelay).Value() == 1);
    REQUIRE(transport->SetOption(t, SocketOption::kKeepAlive, 1).Ok());
    CHECK(transport->GetOption(t, SocketOption::kKeepAlive).Value() == 1);
    CHECK(transport->GetOption(t, SocketOption::kLingerSeconds).Value() == -1);
    REQUIRE(transport->SetOption(t, SocketOption::kLingerSeconds, 3).Ok());
    CHECK(transport->GetOption(t, SocketOption::kLingerSeconds).Value() == 3);
    REQUIRE(transport->SetOption(t, SocketOption::kLingerSeconds, -1).Ok());
    CHECK(transport->GetOption(t, SocketOption::kLingerSeconds).Value() == -1);
    REQUIRE(transport->SetOption(t, SocketOption::kReceiveBuffer, 32768).Ok());
    CHECK(transport->GetOption(t, SocketOption::kReceiveBuffer).Value() >= 32768);
    REQUIRE(transport->SetOption(t, SocketOption::kSendBuffer, 32768).Ok());
    CHECK(transport->GetOption(t, SocketOption::kSendBuffer).Value() >= 32768);

    CHECK(transport->SetOption(u, SocketOption::kNoDelay, 1).Code() == Error::kInvalidArgument);
    CHECK(transport->GetOption(u, SocketOption::kKeepAlive).Code() == Error::kInvalidArgument);
    CHECK(transport->SetOption(u, SocketOption::kLingerSeconds, 0).Code() ==
          Error::kInvalidArgument);
    CHECK(transport->SetOption(t, SocketOption::kBytesReadable, 0).Code() ==
          Error::kInvalidArgument);
    CHECK(transport->SetOption(t, SocketOption::kNonBlocking, 2).Code() ==
          Error::kInvalidArgument);
    closeAll(*transport, {t, u});
}

TEST_CASE("host transport: calls in the wrong state fail with the documented errors") {
    auto transport = makeTransport();
    Bound udp = bindLoopback(*transport, SocketKind::kUdp);
    CHECK(transport->Bind(udp.socket, Endpoint{kLoopback, 0}).Code() == Error::kWrongState);
    CHECK(transport->Listen(udp.socket, 4).Code() == Error::kWrongState);
    std::uint8_t byte = 0;
    CHECK(transport->Send(udp.socket, &byte, 1).Code() == Error::kNotConnected);
    CHECK(transport->Receive(udp.socket, &byte, 1).Code() == Error::kNotConnected);
    CHECK(transport->PeerName(udp.socket).Code() == Error::kNotConnected);
    std::vector<std::uint8_t> huge(65508);
    CHECK(transport->SendTo(udp.socket, huge.data(), huge.size(), udp.local).Code() ==
          Error::kInvalidArgument);
    CHECK(transport->SendTo(udp.socket, &byte, 1, Endpoint{0xFFFFFFFF, udp.local.port}).Code() ==
          Error::kAccessDenied);

    Result<SocketId> unbound = transport->Open(SocketKind::kUdp);
    REQUIRE(unbound.Ok());
    CHECK(transport->ReceiveFrom(unbound.Value(), &byte, 1).Code() == Error::kWrongState);
    CHECK(transport->LocalName(unbound.Value()).Code() == Error::kWrongState);

    Result<SocketId> tcp = transport->Open(SocketKind::kTcp);
    REQUIRE(tcp.Ok());
    CHECK(transport->Listen(tcp.Value(), 4).Code() == Error::kWrongState);
    CHECK(transport->Accept(tcp.Value()).Code() == Error::kWrongState);
    CHECK(transport->Shutdown(tcp.Value(), ShutdownHow::kBoth).Code() == Error::kNotConnected);
    CHECK(transport->Send(tcp.Value(), &byte, 1).Code() == Error::kNotConnected);

    SelectSet nothing;
    CHECK(transport->Select(nothing, 0u).Code() == Error::kInvalidArgument);
    SelectSet dead;
    dead.readable.push_back(SocketId{0xFFFF});
    CHECK(transport->Select(dead, 0u).Code() == Error::kInvalidHandle);
    REQUIRE(dead.readable.size() == 1);
    CHECK(dead.readable[0] == SocketId{0xFFFF});

    closeAll(*transport, {udp.socket, unbound.Value(), tcp.Value()});
    CHECK(transport->Close(udp.socket).Code() == Error::kInvalidHandle);
    CHECK(transport->Send(udp.socket, &byte, 1).Code() == Error::kInvalidHandle);
}

TEST_CASE("host transport: Resolve answers literals, localhost and this machine's name") {
    auto transport = makeTransport();
    Result<std::vector<std::uint32_t>> literal = transport->Resolve("192.0.2.1");
    REQUIRE(literal.Ok());
    REQUIRE(literal.Value().size() == 1);
    CHECK(literal.Value()[0] == 0xC0000201u);

    Result<std::vector<std::uint32_t>> local = transport->Resolve("localhost");
    REQUIRE(local.Ok());
    bool loopback = false;
    for (std::uint32_t address : local.Value()) {
        loopback = loopback || address == kLoopback;
    }
    CHECK(loopback);

    CHECK(transport->Resolve("").Code() == Error::kInvalidArgument);

    Result<std::string> name = transport->HostName();
    REQUIRE(name.Ok());
    CHECK_FALSE(name.Value().empty());
    Result<std::vector<std::uint32_t>> self = transport->Resolve(name.Value());
    REQUIRE(self.Ok());
    CHECK_FALSE(self.Value().empty());

    // The backend itself has no rendezvous; makeTransport()'s wrapper adds it.
    CHECK(makeHostTransport()->GetRendezvous().Code() == Error::kUnsupported);
}

TEST_CASE("host transport: two transports keep separate socket tables") {
    auto first = makeTransport();
    auto second = makeTransport();
    Bound a = bindLoopback(*first, SocketKind::kUdp);
    Bound b = bindLoopback(*second, SocketKind::kUdp);
    const std::uint8_t byte[] = {7};
    REQUIRE(first->SendTo(a.socket, byte, sizeof byte, b.local).Ok());
    std::uint8_t got = 0;
    Result<Received> received = second->ReceiveFrom(b.socket, &got, 1);
    REQUIRE(received.Ok());
    CHECK(got == 7);
    CHECK(received.Value().from == a.local);
    // Ids are numbered per transport, so the first sockets of both share an id. A socket that
    // only the first transport has is unknown to the second.
    Result<SocketId> extra = first->Open(SocketKind::kUdp);
    REQUIRE(extra.Ok());
    CHECK(second->Close(extra.Value()).Code() == Error::kInvalidHandle);
    CHECK(first->Close(extra.Value()).Ok());
    CHECK(first->Close(a.socket).Ok());
    CHECK(second->Close(b.socket).Ok());
}

} // namespace sfcop::platform

#endif // !defined(_WIN32)
