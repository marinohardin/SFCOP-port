// The rendezvous client against the stand-in service, in process: two machines on the in-memory
// network of tools/nettest/, each behind a NAT of its own, on simulated time (5 ms steps).
#include "net/RendezvousEngine.h"

#include "../../tools/nettest/SimNetwork.h"
#include "../../tools/nettest/StandInService.h"

#include <doctest/doctest.h>

#include <functional>
#include <memory>

namespace sfcop::net {
namespace {

using nettest::NatConfig;
using nettest::NatKind;
using nettest::SimNetwork;
using nettest::StandInService;
using platform::Endpoint;
using platform::Error;

constexpr std::uint64_t kSession = 0x5346434F50000001ull;
constexpr std::uint32_t kUnixNow = 1800000000;
constexpr Endpoint kService{0xCB00710A, 47710}; // 203.0.113.10
constexpr std::uint32_t kInsideA = 0xC000020A;  // 192.0.2.10
constexpr std::uint32_t kInsideB = 0xC0000214;  // 192.0.2.20
constexpr std::uint32_t kNatA = 0xC6336401;     // 198.51.100.1
constexpr std::uint32_t kNatB = 0xC6336402;     // 198.51.100.2

nettest::ServiceKey testKey() {
    nettest::ServiceKey key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::uint8_t>(0xA0 + i);
    }
    return key;
}

NatConfig nat(std::uint32_t address, NatKind kind = NatKind::kCone, bool dropPunches = false) {
    NatConfig config;
    config.kind = kind;
    config.externalAddress = address;
    config.dropPunches = dropPunches;
    return config;
}

// Machine A is the host (peer 1), machine B a joiner (peer 2).
struct World {
    SimNetwork network;
    StandInService service{testKey()};
    int hostA = 0;
    int hostB = 0;
    std::unique_ptr<platform::Transport> transportA, transportB;
    std::unique_ptr<RendezvousEngine> a, b;
    std::uint32_t now = 1000;
    std::uint32_t unixNow = kUnixNow; // the service's wall clock, for ticket expiry

    World(std::optional<NatConfig> natA, std::optional<NatConfig> natB, bool forceRelayB = false) {
        network.SetService(kService);
        hostA = natA ? network.AddNattedHost(kInsideA, *natA) : network.AddPublicHost(kNatA);
        hostB = natB ? network.AddNattedHost(kInsideB, *natB) : network.AddPublicHost(kNatB);
        transportA = network.MakeTransport(hostA);
        transportB = network.MakeTransport(hostB);
        RendezvousSettings settings;
        settings.service = kService;
        settings.seed = 100;
        settings.localAddresses = lookUpLocalAddresses(*transportA);
        a = std::make_unique<RendezvousEngine>(*transportA, settings);
        settings.seed = 200;
        settings.forceRelay = forceRelayB;
        settings.localAddresses = lookUpLocalAddresses(*transportB);
        b = std::make_unique<RendezvousEngine>(*transportB, settings);
    }

    Ticket ticket(std::uint32_t peer) {
        return nettest::issueTicket(testKey(), kSession, peer, kUnixNow + 600, peer == 1 ? 1 : 2);
    }

    void pumpService() {
        std::vector<nettest::ServiceDatagram> out;
        while (!network.ServiceInbox().empty()) {
            nettest::SimDatagram datagram = std::move(network.ServiceInbox().front());
            network.ServiceInbox().pop_front();
            service.OnDatagram(datagram.from, datagram.bytes.data(), datagram.bytes.size(), now,
                               unixNow, out);
        }
        service.Tick(now, out);
        for (const nettest::ServiceDatagram &datagram : out) {
            network.SendFromService(datagram.to, datagram.bytes);
        }
    }

    void step() {
        now += 5;
        pumpService();
        a->Pump(now);
        b->Pump(now);
        pumpService();
    }

    // Steps until `done` holds or `limitMs` pass; true if it held.
    bool runUntil(const std::function<bool()> &done, std::uint32_t limitMs) {
        for (std::uint32_t spent = 0; spent <= limitMs; spent += 5) {
            if (done()) {
                return true;
            }
            step();
        }
        return done();
    }

    void bindBoth() {
        REQUIRE(a->StartBind(kSession, 1, ticket(1), now).Ok());
        REQUIRE(b->StartBind(kSession, 2, ticket(2), now).Ok());
        REQUIRE(runUntil(
            [&] {
                return a->Binding() == RendezvousEngine::BindState::kBound &&
                       b->Binding() == RendezvousEngine::BindState::kBound;
            },
            500));
    }

    // B reaches A; returns the time it took and the operation's result.
    std::pair<std::uint32_t, RendezvousEngine::OpStatus> connectBtoA(std::uint32_t limitMs = 6000) {
        const std::uint32_t start = now;
        platform::Result<RendezvousEngine::OpId> op = b->StartConnect(1, now);
        REQUIRE(op.Ok());
        REQUIRE(runUntil([&] { return b->Op(op.Value()).done; }, limitMs));
        RendezvousEngine::OpStatus status = b->Op(op.Value());
        b->FinishOp(op.Value());
        return {now - start, status};
    }

    // Waits until A takes the peer that reached it.
    std::optional<std::uint32_t> acceptOnA(std::uint32_t limitMs = 2000) {
        std::optional<std::uint32_t> peer;
        runUntil(
            [&] {
                peer = a->TakeAccepted();
                return peer.has_value();
            },
            limitMs);
        return peer;
    }

    // One message each way on the open path.
    void exchange() {
        const std::vector<std::uint8_t> hello = {0x48, 0x49};
        REQUIRE(b->Send(1, hello.data(), hello.size(), now).Ok());
        REQUIRE(runUntil([&] { return a->Readable(2); }, 500));
        CHECK(a->TakeMessage(2) == hello);
        const std::vector<std::uint8_t> back(3000, 0x5A); // three fragments
        REQUIRE(a->Send(2, back.data(), back.size(), now).Ok());
        REQUIRE(runUntil([&] { return b->Readable(1); }, 500));
        CHECK(b->TakeMessage(1) == back);
    }
};

} // namespace

TEST_CASE("rendezvous: two machines behind cone NATs exchange candidates and punch directly") {
    World world(nat(kNatA), nat(kNatB));
    world.bindBoth();
    auto [elapsed, status] = world.connectBtoA();
    CHECK(status.error == Error::kNone);
    CHECK(elapsed < 3000);
    MESSAGE("direct path in " << elapsed << " ms of simulated time");
    CHECK(world.service.Introductions() == 1);
    CHECK(world.b->Path(1) == PathKind::kDirect);
    CHECK(world.b->PathEndpoint(1).address == kNatA);
    std::optional<std::uint32_t> accepted = world.acceptOnA();
    REQUIRE(accepted);
    CHECK(*accepted == 2);
    CHECK(world.a->Path(2) == PathKind::kDirect);
    CHECK(world.a->PathEndpoint(2).address == kNatB);
    world.exchange();
    CHECK(world.service.Relayed() == 0);
}

TEST_CASE("rendezvous: a symmetric NAT defeats the punch and the relay opens within 4.5 s") {
    World world(nat(kNatA), nat(kNatB, NatKind::kSymmetric));
    world.bindBoth();
    auto [elapsed, status] = world.connectBtoA();
    CHECK(status.error == Error::kNone);
    CHECK(elapsed >= 3000);
    CHECK(elapsed <= 4500);
    MESSAGE("relay path in " << elapsed << " ms of simulated time");
    CHECK(world.b->Path(1) == PathKind::kRelay);
    CHECK(world.b->PathEndpoint(1) == kService);
    REQUIRE(world.acceptOnA());
    CHECK(world.a->Path(2) == PathKind::kRelay);
    world.exchange();
    CHECK(world.service.Relayed() > 0);
}

TEST_CASE("rendezvous: a NAT that drops the punch sends the pair to the relay") {
    World world(nat(kNatA, NatKind::kCone, true), nat(kNatB));
    world.bindBoth();
    auto [elapsed, status] = world.connectBtoA();
    CHECK(status.error == Error::kNone);
    CHECK(elapsed >= 3000);
    CHECK(elapsed <= 4500);
    CHECK(world.b->Path(1) == PathKind::kRelay);
    CHECK(world.network.Nat(world.hostA).Dropped() > 0);
    REQUIRE(world.acceptOnA());
    world.exchange();
}

TEST_CASE("rendezvous: the test switch sends no probes and takes the relay at once") {
    World world(nat(kNatA), nat(kNatB), true);
    int probesFromB = 0;
    world.network.SetFilter([&](Endpoint from, Endpoint, const std::vector<std::uint8_t> &bytes) {
        if (from.address == kNatB && nettest::isPunch(bytes.data(), bytes.size()) &&
            bytes[5] == static_cast<std::uint8_t>(RvType::kProbe)) {
            ++probesFromB;
        }
        return false;
    });
    world.bindBoth();
    auto [elapsed, status] = world.connectBtoA();
    CHECK(status.error == Error::kNone);
    CHECK(elapsed < 500);
    CHECK(world.b->Path(1) == PathKind::kRelay);
    CHECK(probesFromB == 0);
    REQUIRE(world.acceptOnA());
    CHECK(world.a->Path(2) == PathKind::kRelay); // A chose the path B's ping came on
    world.exchange();
}

TEST_CASE("rendezvous: with no punch and no relay the call fails at 4.5 s on both sides") {
    World world(nat(kNatA), nat(kNatB, NatKind::kSymmetric));
    world.service.SetRelayEnabled(false);
    world.bindBoth();
    auto [elapsed, status] = world.connectBtoA();
    CHECK(status.error == Error::kHostUnreachable);
    CHECK(elapsed >= 4500);
    CHECK(elapsed < 4600);
    REQUIRE(world.runUntil([&] { return world.a->AcceptFailures() == 1; }, 500));
    CHECK_FALSE(world.a->TakeAccepted());
}

TEST_CASE("rendezvous: an unbound peer is kHostUnreachable and a forgotten session kNotFound") {
    World world(nat(kNatA), nat(kNatB));
    world.bindBoth();
    platform::Result<RendezvousEngine::OpId> op = world.b->StartConnect(3, world.now);
    REQUIRE(op.Ok());
    REQUIRE(world.runUntil([&] { return world.b->Op(op.Value()).done; }, 500));
    CHECK(world.b->Op(op.Value()).error == Error::kHostUnreachable);
    world.b->FinishOp(op.Value());

    world.service.Restart(); // forgets every binding: the session is unknown
    auto [elapsed, status] = world.connectBtoA();
    CHECK(status.error == Error::kNotFound);
    CHECK(elapsed < 500);
}

TEST_CASE("rendezvous: kUnknownBinding is answered by binding again, and the call goes on") {
    World world(nat(kNatA), nat(kNatB));
    world.bindBoth();
    world.service.Restart();
    // A binds again on its own schedule; B has not yet, so the service no longer knows B.
    REQUIRE(world.a->StartBind(kSession, 1, world.ticket(1), world.now).Ok());
    REQUIRE(world.runUntil(
        [&] { return world.a->Binding() == RendezvousEngine::BindState::kBound; }, 500));
    auto [elapsed, status] = world.connectBtoA();
    CHECK(status.error == Error::kNone);
    CHECK(world.b->Path(1) == PathKind::kDirect);
    CHECK(world.service.Bindings() == 2);
}

TEST_CASE("rendezvous: a rebind that fails reports the Bind's own error to the waiting call") {
    World world(nat(kNatA), nat(kNatB));
    world.bindBoth();
    world.service.Restart();
    REQUIRE(world.a->StartBind(kSession, 1, world.ticket(1), world.now).Ok());
    REQUIRE(world.runUntil(
        [&] { return world.a->Binding() == RendezvousEngine::BindState::kBound; }, 500));
    world.unixNow = kUnixNow + 700; // B's ticket has expired: its rebind is refused
    auto [elapsed, status] = world.connectBtoA();
    (void)elapsed;
    CHECK(status.error == Error::kAccessDenied);
    CHECK(world.b->BindError() == Error::kAccessDenied);
}

TEST_CASE("rendezvous: no answer from the service is kTimedOut after 500 ms") {
    World world(nat(kNatA), nat(kNatB));
    world.bindBoth();
    world.network.SetFilter([](Endpoint, Endpoint to, const std::vector<std::uint8_t> &) {
        return to == kService; // the service is out of reach from here on
    });
    auto [elapsed, status] = world.connectBtoA();
    CHECK(status.error == Error::kTimedOut);
    CHECK(elapsed >= 500);
    CHECK(elapsed < 600);

    REQUIRE(world.b->StartBind(kSession, 2, world.ticket(2), world.now).Ok());
    REQUIRE(world.runUntil(
        [&] { return world.b->Binding() != RendezvousEngine::BindState::kBinding; }, 1000));
    CHECK(world.b->Binding() == RendezvousEngine::BindState::kFailed);
    CHECK(world.b->BindError() == Error::kTimedOut);
}

TEST_CASE("rendezvous: a ticket with a wrong mac or an expired one is kAccessDenied") {
    World world(nat(kNatA), nat(kNatB));
    Ticket forged = world.ticket(2);
    forged[35] ^= 0x01;
    REQUIRE(world.b->StartBind(kSession, 2, forged, world.now).Ok());
    REQUIRE(world.runUntil(
        [&] { return world.b->Binding() != RendezvousEngine::BindState::kBinding; }, 500));
    CHECK(world.b->BindError() == Error::kAccessDenied);

    Ticket expired = nettest::issueTicket(testKey(), kSession, 2, kUnixNow - 1, 2);
    REQUIRE(world.b->StartBind(kSession, 2, expired, world.now).Ok());
    REQUIRE(world.runUntil(
        [&] { return world.b->Binding() != RendezvousEngine::BindState::kBinding; }, 500));
    CHECK(world.b->BindError() == Error::kAccessDenied);
    CHECK(world.service.Bindings() == 0);
}

TEST_CASE("rendezvous: two machines that reach each other at once both succeed, unaccepted") {
    World world(nat(kNatA), nat(kNatB));
    world.bindBoth();
    platform::Result<RendezvousEngine::OpId> fromA = world.a->StartConnect(2, world.now);
    platform::Result<RendezvousEngine::OpId> fromB = world.b->StartConnect(1, world.now);
    REQUIRE(fromA.Ok());
    REQUIRE(fromB.Ok());
    REQUIRE(world.runUntil(
        [&] { return world.a->Op(fromA.Value()).done && world.b->Op(fromB.Value()).done; }, 5000));
    CHECK(world.a->Op(fromA.Value()).error == Error::kNone);
    CHECK(world.b->Op(fromB.Value()).error == Error::kNone);
    world.a->FinishOp(fromA.Value());
    world.b->FinishOp(fromB.Value());
    world.runUntil([] { return false; }, 1000);
    CHECK_FALSE(world.a->TakeAccepted());
    CHECK_FALSE(world.b->TakeAccepted());
    world.exchange();
}

TEST_CASE("rendezvous: an envelope from an address that is not on the path is refused") {
    World world(std::nullopt, std::nullopt);                      // both public: nothing filters
    const int stranger = world.network.AddPublicHost(0xCB007163); // 203.0.113.99
    std::unique_ptr<platform::Transport> other = world.network.MakeTransport(stranger);
    world.bindBoth();
    auto [elapsed, status] = world.connectBtoA();
    (void)elapsed;
    REQUIRE(status.error == Error::kNone);
    REQUIRE(world.acceptOnA());
    REQUIRE(world.b->Path(1) == PathKind::kDirect);

    Envelope forged;
    forged.header.kind = EnvelopeKind::kData;
    forged.header.sessionId = kSession;
    forged.header.source = 2;
    forged.header.destination = 1;
    forged.header.sequence = 77;
    forged.payload = {0x66};
    std::vector<std::uint8_t> bytes = encodeEnvelope(forged);
    platform::Result<platform::SocketId> socket = other->Open(platform::SocketKind::kUdp);
    REQUIRE(socket.Ok());
    const std::uint32_t refusedBefore = world.a->Refused();
    REQUIRE(
        other->SendTo(socket.Value(), bytes.data(), bytes.size(), world.b->PathEndpoint(1)).Ok());
    world.step();
    CHECK(world.a->Refused() == refusedBefore + 1);
    CHECK_FALSE(world.a->Readable(2));
}

TEST_CASE("rendezvous: the binding is refreshed every 15 s and paths stay alive") {
    World world(nat(kNatA), nat(kNatB));
    world.bindBoth();
    auto [elapsed, status] = world.connectBtoA();
    (void)elapsed;
    REQUIRE(status.error == Error::kNone);
    REQUIRE(world.acceptOnA());
    world.runUntil([] { return false; }, 60000); // past the service's 45 s lifetime
    CHECK(world.service.Bindings() == 2);
    CHECK_FALSE(world.a->Gone(2, world.now));
    CHECK_FALSE(world.b->Gone(1, world.now));
    world.exchange();
}

TEST_CASE("rendezvous: closing one side's socket closes the path on the other") {
    World world(nat(kNatA), nat(kNatB));
    world.bindBoth();
    auto [elapsed, status] = world.connectBtoA();
    (void)elapsed;
    REQUIRE(status.error == Error::kNone);
    REQUIRE(world.acceptOnA());
    world.b->Release(1, world.now);
    REQUIRE(world.runUntil([&] { return world.a->Gone(2, world.now); }, 500));
    const std::vector<std::uint8_t> one = {0x01};
    CHECK(world.a->Send(2, one.data(), 1, world.now).Code() == Error::kConnectionReset);
    CHECK(world.runUntil([&] { return world.b->Path(1) == PathKind::kNone; }, 1000));
}

TEST_CASE("rendezvous: calls before a binding, to oneself, or twice to one peer are refused") {
    World world(nat(kNatA), nat(kNatB));
    CHECK(world.b->StartConnect(1, world.now).Code() == Error::kWrongState);
    CHECK(world.b->Unbind(world.now).Code() == Error::kWrongState);
    world.bindBoth();
    CHECK(world.b->StartConnect(2, world.now).Code() == Error::kInvalidArgument);
    CHECK(world.b->StartConnect(0, world.now).Code() == Error::kInvalidArgument);
    REQUIRE(world.b->StartConnect(1, world.now).Ok());
    CHECK(world.b->StartConnect(1, world.now).Code() == Error::kWrongState);
    CHECK(world.b->Unbind(world.now).Ok());
    CHECK(world.b->Binding() == RendezvousEngine::BindState::kUnbound);
    world.pumpService();
    CHECK(world.service.Bindings() == 1);
}

TEST_CASE("rendezvous: Unbind keeps the open path; only new calls are refused") {
    World world(nat(kNatA), nat(kNatB));
    world.bindBoth();
    auto [elapsed, status] = world.connectBtoA();
    (void)elapsed;
    REQUIRE(status.error == Error::kNone);
    REQUIRE(world.acceptOnA());
    REQUIRE(world.b->Unbind(world.now).Ok());
    REQUIRE(world.a->Unbind(world.now).Ok());
    world.exchange();
    world.runUntil([] { return false; }, 12000); // past the 10 s liveness limit
    CHECK_FALSE(world.a->Gone(2, world.now));
    CHECK_FALSE(world.b->Gone(1, world.now));
    world.exchange();
    CHECK(world.b->StartConnect(1, world.now).Code() == Error::kWrongState);
    CHECK_FALSE(world.b->Idle()); // the open peer still needs pumping
}

TEST_CASE("rendezvous: with no binding and no peer the engine is idle") {
    World world(nat(kNatA), nat(kNatB));
    CHECK(world.b->Idle());
    REQUIRE(world.b->StartBind(kSession, 2, world.ticket(2), world.now).Ok());
    CHECK_FALSE(world.b->Idle());
    REQUIRE(world.runUntil(
        [&] { return world.b->Binding() == RendezvousEngine::BindState::kBound; }, 500));
    CHECK_FALSE(world.b->Idle());
    REQUIRE(world.b->Unbind(world.now).Ok());
    CHECK(world.b->Idle());
}

TEST_CASE("rendezvous: a flood of datagrams is read over several pumps") {
    World world(nat(kNatA), nat(kNatB));
    world.bindBoth();
    platform::Result<Endpoint> local = world.transportA->LocalName(world.a->Socket());
    REQUIRE(local.Ok());
    const std::uint32_t refusedBefore = world.a->Refused();
    for (int i = 0; i < 200; ++i) {
        world.network.Inbox(world.hostA, local.Value().port)
            .push_back(nettest::SimDatagram{kService, {0x01, 0x02, 0x03}});
    }
    world.a->Pump(world.now);
    CHECK(world.a->Refused() == refusedBefore + 64);
    world.a->Pump(world.now);
    world.a->Pump(world.now);
    world.a->Pump(world.now);
    CHECK(world.a->Refused() == refusedBefore + 200);
}

} // namespace sfcop::net
