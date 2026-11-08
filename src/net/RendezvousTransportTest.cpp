// The wrapper's own rules, on the transport mock: what it passes through, what it refuses before
// it touches the network. The paths themselves are tested by RendezvousEngineTest (simulated) and
// NetPairTest (two processes on real sockets).
#include "net/RendezvousTransport.h"

#include "net/PeerNames.h"

#include "../../tools/nettest/StandInService.h"
#include "platform/Transport/TransportHost.h"
#include "platform/Transport/TransportMock.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

namespace sfcop::net {
namespace {

using platform::Error;
using platform::SocketId;
using platform::TransportMock;

struct Wrapped {
    TransportMock *mock;
    std::unique_ptr<platform::Transport> transport;
};

Wrapped wrap(const std::string &service) {
    auto inner = std::make_unique<TransportMock>();
    TransportMock *mock = inner.get();
    RendezvousConfig config;
    if (!service.empty()) {
        REQUIRE(parseServiceAddress(service, config));
    }
    return Wrapped{mock, makeRendezvousTransport(std::move(inner), config)};
}

const std::uint8_t kTicket[36] = {};

} // namespace

TEST_CASE("peer names: a session alone names the host, a session and a peer name that peer") {
    std::optional<PeerName> host = parsePeerName("5346434f50000001");
    REQUIRE(host);
    CHECK(host->session == 0x5346434F50000001ull);
    CHECK(host->peer == 1);
    std::optional<PeerName> peer = parsePeerName("5346434f50000001:0000000a");
    REQUIRE(peer);
    CHECK(peer->peer == 10);
    CHECK(formatPeerName(*peer) == "5346434f50000001:0000000a");

    CHECK_FALSE(parsePeerName("5346434F50000001"));           // upper case
    CHECK_FALSE(parsePeerName("5346434f5000001"));            // 15 digits
    CHECK_FALSE(parsePeerName("5346434f50000001:"));          // no peer
    CHECK_FALSE(parsePeerName("5346434f50000001:00000000"));  // peer 0
    CHECK_FALSE(parsePeerName("5346434f50000001:000000001")); // 9 digits
    CHECK_FALSE(parsePeerName("5346434f50000001-00000002"));
    CHECK_FALSE(parsePeerName(""));
    CHECK(parseSessionId("0123456789abcdef").value() == 0x0123456789ABCDEFull);
}

TEST_CASE("service address: host, or host and port; anything else is refused") {
    RendezvousConfig config;
    REQUIRE(parseServiceAddress("203.0.113.10", config));
    CHECK(config.serviceHost == "203.0.113.10");
    CHECK(config.servicePort == 47710);
    REQUIRE(parseServiceAddress("rendezvous.example:40001", config));
    CHECK(config.serviceHost == "rendezvous.example");
    CHECK(config.servicePort == 40001);
    RendezvousConfig unchanged;
    CHECK_FALSE(parseServiceAddress("", unchanged));
    CHECK_FALSE(parseServiceAddress(":47710", unchanged));
    CHECK_FALSE(parseServiceAddress("host:", unchanged));
    CHECK_FALSE(parseServiceAddress("host:0", unchanged));
    CHECK_FALSE(parseServiceAddress("host:65536", unchanged));
    CHECK_FALSE(parseServiceAddress("host:4x", unchanged));
    CHECK(unchanged.serviceHost.empty());
}

TEST_CASE("rendezvous transport: with no service configured there is no Rendezvous") {
    Wrapped wrapped = wrap("");
    CHECK(wrapped.transport->GetRendezvous().Code() == Error::kUnsupported);
}

TEST_CASE("rendezvous transport: the backend's sockets pass through unchanged") {
    Wrapped wrapped = wrap("203.0.113.10");
    wrapped.mock->openResult = SocketId{7};
    REQUIRE(wrapped.transport->Open(platform::SocketKind::kUdp).Value() == SocketId{7});
    const std::uint8_t byte = 1;
    CHECK(wrapped.transport->SendTo(SocketId{7}, &byte, 1, platform::Endpoint{0xC0000201, 9})
              .Value() == 1);
    CHECK(wrapped.transport->Close(SocketId{7}).Ok());
    CHECK(wrapped.mock->log.Calls() ==
          std::vector<std::string>{"Open(0)", "SendTo(7, 1, 3221225985, 9)", "Close(7)"});
    platform::SelectSet sets;
    sets.readable.push_back(SocketId{7});
    wrapped.mock->selectReady = sets;
    CHECK(wrapped.transport->Select(sets, 0).Value() == 1);
    CHECK(wrapped.mock->log.Count("Select") == 1);
}

TEST_CASE("rendezvous transport: a backend id with bit 31 set is closed and refused") {
    Wrapped wrapped = wrap("203.0.113.10");
    wrapped.mock->openResult = SocketId{0x80000001};
    CHECK(wrapped.transport->Open(platform::SocketKind::kUdp).Code() == Error::kOutOfResources);
    CHECK(wrapped.mock->log.Count("Close") == 1);
}

TEST_CASE("rendezvous transport: malformed arguments are refused before the network is used") {
    Wrapped wrapped = wrap("203.0.113.10");
    platform::Rendezvous *rendezvous = wrapped.transport->GetRendezvous().Value();
    CHECK(rendezvous->Bind("5346434F50000001", 2, kTicket, 36, 500).Code() ==
          Error::kInvalidArgument);
    CHECK(rendezvous->Bind("5346434f50000001", 0, kTicket, 36, 500).Code() ==
          Error::kInvalidArgument);
    CHECK(rendezvous->Bind("5346434f50000001", 2, kTicket, 35, 500).Code() ==
          Error::kInvalidArgument);
    CHECK(rendezvous->Bind("5346434f50000001", 2, nullptr, 36, 500).Code() ==
          Error::kInvalidArgument);
    CHECK(rendezvous->ConnectToPeer("not a name", 500).Code() == Error::kInvalidArgument);
    CHECK(rendezvous->ConnectToPeer("5346434f50000001", 500).Code() == Error::kWrongState);
    CHECK(rendezvous->AcceptPeer(500).Code() == Error::kWrongState);
    CHECK(rendezvous->Unbind().Code() == Error::kWrongState);
    CHECK(wrapped.mock->log.Calls().empty());
}

TEST_CASE("rendezvous transport: a peer socket id it never handed out is an invalid handle") {
    Wrapped wrapped = wrap("203.0.113.10");
    const SocketId unknown{0x80000005};
    std::uint8_t byte = 0;
    CHECK(wrapped.transport->Close(unknown).Code() == Error::kInvalidHandle);
    CHECK(wrapped.transport->Send(unknown, &byte, 1).Code() == Error::kInvalidHandle);
    CHECK(wrapped.transport->SetOption(unknown, platform::SocketOption::kNonBlocking, 1).Code() ==
          Error::kInvalidHandle);
    CHECK(wrapped.transport->PeerName(unknown).Code() == Error::kInvalidHandle);
    platform::SelectSet sets;
    sets.readable.push_back(unknown);
    CHECK(wrapped.transport->Select(sets, 0).Code() == Error::kInvalidHandle);
    CHECK(wrapped.mock->log.Calls().empty());
}

#if !defined(_WIN32)
// Waits up to `limitMs` of real time for `done`; generous, so a loaded machine does not fail it.
template <typename Done> bool within(std::uint32_t limitMs, Done done) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(limitMs);
    while (!done()) {
        if (std::chrono::steady_clock::now() >= until) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

// The stand-in rendezvous and relay service on a loopback UDP socket, on a thread of its own.
class LoopbackService {
public:
    LoopbackService() {
        nettest::ServiceKey key{};
        for (std::size_t i = 0; i < key.size(); ++i) {
            key[i] = static_cast<std::uint8_t>(0x60 + i);
        }
        key_ = key;
        service_ = std::make_unique<nettest::StandInService>(key);
        platform::Result<SocketId> socket = backend_->Open(platform::SocketKind::kUdp);
        REQUIRE(socket.Ok());
        socket_ = socket.Value();
        REQUIRE(backend_->SetOption(socket_, platform::SocketOption::kNonBlocking, 1).Ok());
        REQUIRE(backend_->Bind(socket_, platform::Endpoint{0x7F000001, 0}).Ok());
        port_ = backend_->LocalName(socket_).Value().port;
        thread_ = std::thread([this] { run(); });
    }
    ~LoopbackService() {
        stop_ = true;
        thread_.join();
        (void)backend_->Close(socket_);
    }

    std::uint16_t Port() const { return port_; }
    Ticket TicketFor(std::uint32_t peer) const {
        const auto unixNow = static_cast<std::uint32_t>(std::time(nullptr));
        return nettest::issueTicket(key_, kSession, peer, unixNow + 600, peer == 1 ? 1 : 2);
    }

    static constexpr std::uint64_t kSession = 0x5346434F50000001ull;

private:
    std::unique_ptr<platform::Transport> backend_ = platform::makeHostTransport();
    nettest::ServiceKey key_{};
    std::unique_ptr<nettest::StandInService> service_;
    SocketId socket_;
    std::uint16_t port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;

    void run() {
        const auto start = std::chrono::steady_clock::now();
        std::uint8_t buffer[2048];
        while (!stop_) {
            platform::SelectSet sets;
            sets.readable.push_back(socket_);
            (void)backend_->Select(sets, 5);
            const auto nowMs =
                static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::steady_clock::now() - start)
                                               .count());
            std::vector<nettest::ServiceDatagram> out;
            for (;;) {
                platform::Result<platform::Received> got =
                    backend_->ReceiveFrom(socket_, buffer, sizeof buffer);
                if (!got.Ok()) {
                    break;
                }
                service_->OnDatagram(got.Value().from, buffer, got.Value().bytes, nowMs,
                                     static_cast<std::uint32_t>(std::time(nullptr)), out);
            }
            service_->Tick(nowMs, out);
            for (const nettest::ServiceDatagram &datagram : out) {
                (void)backend_->SendTo(socket_, datagram.bytes.data(), datagram.bytes.size(),
                                       datagram.to);
            }
        }
    }
};

std::unique_ptr<platform::Transport> clientOf(const LoopbackService &service) {
    RendezvousConfig config;
    REQUIRE(parseServiceAddress("127.0.0.1:" + std::to_string(service.Port()), config));
    config.localCandidates = false;
    return makeRendezvousTransport(platform::makeHostTransport(), config);
}

TEST_CASE("rendezvous transport: sockets handed out keep working after Unbind on both sides") {
    LoopbackService service;
    std::unique_ptr<platform::Transport> host = clientOf(service);
    std::unique_ptr<platform::Transport> joiner = clientOf(service);
    platform::Rendezvous *hostRv = host->GetRendezvous().Value();
    platform::Rendezvous *joinerRv = joiner->GetRendezvous().Value();
    const Ticket hostTicket = service.TicketFor(1);
    const Ticket joinerTicket = service.TicketFor(2);
    REQUIRE(hostRv->Bind("5346434f50000001", 1, hostTicket.data(), hostTicket.size(), 2000).Ok());
    REQUIRE(
        joinerRv->Bind("5346434f50000001", 2, joinerTicket.data(), joinerTicket.size(), 2000).Ok());

    platform::Result<SocketId> accepted = Error::kTimedOut;
    std::thread accepting([&] { accepted = hostRv->AcceptPeer(8000); });
    platform::Result<SocketId> connected = joinerRv->ConnectToPeer("5346434f50000001", 8000);
    accepting.join();
    REQUIRE(connected.Ok());
    REQUIRE(accepted.Ok());

    REQUIRE(hostRv->Unbind().Ok());
    REQUIRE(joinerRv->Unbind().Ok());

    const std::uint8_t there[] = {0x54, 0x48, 0x45, 0x52, 0x45};
    const std::uint8_t back[] = {0x42, 0x41, 0x43, 0x4B};
    REQUIRE(joiner->Send(connected.Value(), there, sizeof there).Ok());
    REQUIRE(host->SetOption(accepted.Value(), platform::SocketOption::kNonBlocking, 1).Ok());
    REQUIRE(joiner->SetOption(connected.Value(), platform::SocketOption::kNonBlocking, 1).Ok());
    std::uint8_t buffer[16] = {};
    platform::Result<std::size_t> got = Error::kWouldBlock;
    CHECK(within(5000, [&] {
        got = host->Receive(accepted.Value(), buffer, sizeof buffer);
        return got.Ok();
    }));
    REQUIRE(got.Ok());
    CHECK(std::vector<std::uint8_t>(buffer, buffer + got.Value()) ==
          std::vector<std::uint8_t>(there, there + sizeof there));

    REQUIRE(host->Send(accepted.Value(), back, sizeof back).Ok());
    CHECK(within(5000, [&] {
        got = joiner->Receive(connected.Value(), buffer, sizeof buffer);
        return got.Ok();
    }));
    REQUIRE(got.Ok());
    CHECK(std::vector<std::uint8_t>(buffer, buffer + got.Value()) ==
          std::vector<std::uint8_t>(back, back + sizeof back));

    CHECK(joinerRv->ConnectToPeer("5346434f50000001", 1000).Code() == Error::kWrongState);
    (void)joiner->Close(connected.Value());
    (void)host->Close(accepted.Value());
}

TEST_CASE("rendezvous transport: the service thread parks when there is nothing to pump") {
    // A service on loopback that never answers: Bind() times out, the binding is dropped, and
    // with no peer the thread has nothing to do.
    std::unique_ptr<platform::Transport> backend = platform::makeHostTransport();
    platform::Result<SocketId> silent = backend->Open(platform::SocketKind::kUdp);
    REQUIRE(silent.Ok());
    REQUIRE(backend->Bind(silent.Value(), platform::Endpoint{0x7F000001, 0}).Ok());
    const std::uint16_t port = backend->LocalName(silent.Value()).Value().port;

    RendezvousConfig config;
    REQUIRE(parseServiceAddress("127.0.0.1:" + std::to_string(port), config));
    config.localCandidates = false;
    std::unique_ptr<platform::Transport> transport =
        makeRendezvousTransport(platform::makeHostTransport(), config);
    platform::Result<platform::Rendezvous *> rendezvous = transport->GetRendezvous();
    REQUIRE(rendezvous.Ok());
    CHECK(rendezvousPumps(*transport) == 0); // no thread before the first Bind()

    CHECK(rendezvous.Value()->Bind("5346434f50000001", 2, kTicket, sizeof kTicket, 100).Code() ==
          Error::kTimedOut);
    // Parked: within 1 s the count holds still for 200 ms, where a running thread pumps every
    // 10 ms (20 times).
    std::uint64_t parked = 0;
    CHECK(within(1000, [&] {
        const std::uint64_t before = rendezvousPumps(*transport);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        parked = rendezvousPumps(*transport);
        return before > 0 && parked == before;
    }));

    // A new Bind() wakes it.
    CHECK(rendezvous.Value()->Bind("5346434f50000001", 2, kTicket, sizeof kTicket, 50).Code() ==
          Error::kTimedOut);
    CHECK(within(1000, [&] { return rendezvousPumps(*transport) > parked; }));
    (void)backend->Close(silent.Value());
}
#endif

} // namespace sfcop::net
