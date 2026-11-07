#include "platform/Transport/Transport.h"
#include "platform/Transport/TransportMock.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace sfcop::platform {
namespace {

// RFC 5737 test addresses.
constexpr std::uint32_t kTestNet1Host = 0xC0000201; // 192.0.2.1
constexpr std::uint32_t kTestNet2Host = 0xC6336402; // 198.51.100.2

// A caller written against the interface only, as a game's LAN discovery is: opens a broadcast
// UDP socket, sends a probe, waits for replies, and collects who answered.
Result<std::vector<Endpoint>> discover(Transport &transport, std::uint16_t port) {
    Result<SocketId> socket = transport.Open(SocketKind::kUdp);
    if (!socket.Ok()) {
        return socket.Code();
    }
    SocketId id = socket.Value();
    std::vector<Endpoint> answered;
    const std::uint8_t probe[] = {'P', 'I', 'N', 'G'};
    Status configured = transport.SetOption(id, SocketOption::kBroadcast, 1);
    Result<std::size_t> sent = Error::kWrongState;
    if (configured.Ok()) {
        sent = transport.SendTo(id, probe, sizeof probe, Endpoint{0xFFFFFFFF, port});
    }
    if (sent.Ok()) {
        SelectSet sets;
        sets.readable.push_back(id);
        Result<std::size_t> ready = transport.Select(sets, 100u);
        while (ready.Ok() && ready.Value() > 0) {
            std::uint8_t reply[16];
            Result<Received> got = transport.ReceiveFrom(id, reply, sizeof reply);
            if (!got.Ok()) {
                break;
            }
            answered.push_back(got.Value().from);
        }
    }
    (void)transport.Close(id);
    if (!configured.Ok()) {
        return configured.Code();
    }
    if (!sent.Ok()) {
        return sent.Code();
    }
    return answered;
}

} // namespace

TEST_CASE("a caller broadcasts a probe and collects the replies through the transport interface") {
    TransportMock mock;
    mock.openResult = SocketId{7};
    mock.selectReady.readable.push_back(SocketId{7});
    mock.incoming.push_back(MockDatagram{{'P', 'O', 'N', 'G'}, Endpoint{kTestNet1Host, 26900}});
    mock.incoming.push_back(MockDatagram{{'P', 'O', 'N', 'G'}, Endpoint{kTestNet2Host, 26900}});

    Result<std::vector<Endpoint>> found = discover(mock, 26900);
    REQUIRE(found.Ok());
    CHECK(found.Value() == std::vector<Endpoint>{Endpoint{kTestNet1Host, 26900},
                                                 Endpoint{kTestNet2Host, 26900}});
    REQUIRE(mock.sent.size() == 1);
    CHECK(mock.sent[0].bytes == std::vector<std::uint8_t>{'P', 'I', 'N', 'G'});
    CHECK(mock.sent[0].peer == Endpoint{0xFFFFFFFF, 26900});
    CHECK(mock.log.Calls() == std::vector<std::string>{
                                  "Open(0)",
                                  "SetOption(7, 1, 1)",
                                  "SendTo(7, 4, 4294967295, 26900)",
                                  "Select(1, 0, 0, 100)",
                                  "ReceiveFrom(7, 16)",
                                  "ReceiveFrom(7, 16)",
                                  "ReceiveFrom(7, 16)",
                                  "Close(7)",
                              });
}

TEST_CASE("a refused broadcast reaches the caller and the socket is still closed") {
    TransportMock mock;
    mock.sendError = Error::kAccessDenied;
    CHECK(discover(mock, 26900).Code() == Error::kAccessDenied);
    CHECK(mock.log.Count("Close") == 1);
    CHECK(mock.log.Count("Select") == 0);
}

TEST_CASE("the transport mock hands out its rendezvous until told it has none") {
    TransportMock mock;
    Transport &transport = mock;
    Result<Rendezvous *> rendezvous = transport.GetRendezvous();
    REQUIRE(rendezvous.Ok());
    const std::uint8_t ticket[36] = {};
    CHECK(rendezvous.Value()->Bind("5346434f50000001", 2, ticket, sizeof ticket, 500).Ok());
    CHECK(rendezvous.Value()->ConnectToPeer("5346434f50000001", 5000).Value() == SocketId{100});
    CHECK(rendezvous.Value()->AcceptPeer(5000).Value() == SocketId{101});
    CHECK(rendezvous.Value()->Unbind().Ok());
    CHECK(mock.rendezvous.log.Calls() ==
          std::vector<std::string>{"Bind(\"5346434f50000001\", 2, 36, 500)",
                                   "ConnectToPeer(\"5346434f50000001\", 5000)",
                                   "AcceptPeer(5000)", "Unbind()"});
    mock.rendezvous.bindResult = Error::kAccessDenied;
    CHECK(rendezvous.Value()->Bind("5346434f50000001", 2, ticket, sizeof ticket, 500).Code() ==
          Error::kAccessDenied);
    mock.rendezvousAvailable = false;
    CHECK(transport.GetRendezvous().Code() == Error::kUnsupported);
}

TEST_CASE("every method of the transport stub reports unsupported") {
    std::unique_ptr<Transport> transport = makeTransportStub();
    SocketId socket{1};
    std::uint8_t buffer[4] = {};
    SelectSet sets;
    sets.readable.push_back(socket);
    Endpoint peer{kTestNet1Host, 26900};
    CHECK(transport->Open(SocketKind::kTcp).Code() == Error::kUnsupported);
    CHECK(transport->Close(socket).Code() == Error::kUnsupported);
    CHECK(transport->SetOption(socket, SocketOption::kNonBlocking, 1).Code() ==
          Error::kUnsupported);
    CHECK(transport->GetOption(socket, SocketOption::kBytesReadable).Code() ==
          Error::kUnsupported);
    CHECK(transport->Bind(socket, Endpoint{}).Code() == Error::kUnsupported);
    CHECK(transport->Connect(socket, peer).Code() == Error::kUnsupported);
    CHECK(transport->Listen(socket, 5).Code() == Error::kUnsupported);
    CHECK(transport->Accept(socket).Code() == Error::kUnsupported);
    CHECK(transport->Send(socket, buffer, 4).Code() == Error::kUnsupported);
    CHECK(transport->Receive(socket, buffer, 4).Code() == Error::kUnsupported);
    CHECK(transport->SendTo(socket, buffer, 4, peer).Code() == Error::kUnsupported);
    CHECK(transport->ReceiveFrom(socket, buffer, 4).Code() == Error::kUnsupported);
    CHECK(transport->Shutdown(socket, ShutdownHow::kBoth).Code() == Error::kUnsupported);
    CHECK(transport->PeerName(socket).Code() == Error::kUnsupported);
    CHECK(transport->LocalName(socket).Code() == Error::kUnsupported);
    CHECK(transport->Select(sets, 0u).Code() == Error::kUnsupported);
    CHECK(transport->Resolve("test.invalid").Code() == Error::kUnsupported);
    CHECK(transport->HostName().Code() == Error::kUnsupported);
    CHECK(transport->GetRendezvous().Code() == Error::kUnsupported);
}

} // namespace sfcop::platform
