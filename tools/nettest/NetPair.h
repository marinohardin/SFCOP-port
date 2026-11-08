// The two-process test of the Rendezvous on one machine (N2, clause 3). The parent runs the
// stand-in service (StandInService.h) and one simulated NAT per client on loopback; it forks two
// clients, a host and a joiner, each its own process with the real host backend
// (makeTransport()) under net::RendezvousTransport, as the native client has it. The joiner calls
// ConnectToPeer() on the host's name, the host AcceptPeer(), and the two exchange one message
// each way; the joiner reports the path the Rendezvous took and how long ConnectToPeer() took.
//
// The NAT is a UDP forwarder. Each client's Transport is wrapped in a tunnel that sends every
// datagram to its NAT's inside port with the real destination in front (6 bytes: the IPv4
// address and the port, little-endian), and takes the source off what comes back. The NAT
// rewrites the source as NatModel.h says, from one real loopback socket per mapping, and filters
// what comes in. So every datagram a client sends or receives crosses its NAT, as on a real
// network, while both run on one machine. The clients offer no local candidates: on one machine
// they would reach each other around the NATs.
//
// Real sockets and the steady clock, each step bounded by a deadline: like
// src/hle/dplay/tools/LanPairTest.cpp, the one place a test may use them (CONVENTIONS §6). POSIX
// only (fork()); nothing identifying is printed (every address is loopback, and none is logged).
#pragma once

#if !defined(_WIN32)

#include "net/PeerNames.h"
#include "net/RendezvousTransport.h"
#include "platform/Transport/Transport.h"

#include "NatModel.h"
#include "StandInService.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace sfcop::nettest {

enum class Scenario : std::uint8_t {
    kCone,      // both NATs cone: the punch works
    kSymmetric, // the joiner's NAT is symmetric: the punch fails, the relay is taken
    kBlocked,   // both cone, but the host's NAT drops every Probe and ProbeReply
    kForced,    // both cone, the joiner's test switch forces the relay (RENDEZVOUS.md step 7)
};

inline const char *scenarioName(Scenario scenario) {
    switch (scenario) {
    case Scenario::kCone:
        return "cone";
    case Scenario::kSymmetric:
        return "symmetric";
    case Scenario::kBlocked:
        return "blocked";
    case Scenario::kForced:
        return "forced";
    }
    return "?";
}

struct PairResult {
    bool ok = false;
    std::string path; // "direct" or "relay"
    std::uint32_t connectMs = 0;
    std::string failure; // why, when not ok
};

namespace detail {

constexpr std::uint32_t kLoopback = 0x7F000001;
constexpr std::size_t kPrefix = 6;

inline void putPrefix(std::vector<std::uint8_t> &out, platform::Endpoint endpoint) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(endpoint.address >> (8 * i)));
    }
    out.push_back(static_cast<std::uint8_t>(endpoint.port));
    out.push_back(static_cast<std::uint8_t>(endpoint.port >> 8));
}

inline platform::Endpoint getPrefix(const std::uint8_t *data) {
    platform::Endpoint endpoint;
    for (int i = 0; i < 4; ++i) {
        endpoint.address |= static_cast<std::uint32_t>(data[i]) << (8 * i);
    }
    endpoint.port = static_cast<std::uint16_t>(data[4] | (data[5] << 8));
    return endpoint;
}

// A client's Transport with every UDP datagram tunnelled through its NAT (see the file comment).
class TunnelTransport final : public platform::Transport {
public:
    TunnelTransport(std::unique_ptr<platform::Transport> inner, platform::Endpoint nat)
        : inner_(std::move(inner)), nat_(nat) {}

    platform::Result<std::size_t> SendTo(platform::SocketId id, const void *data, std::size_t bytes,
                                         platform::Endpoint peer) override {
        std::vector<std::uint8_t> wrapped;
        putPrefix(wrapped, peer);
        const auto *first = static_cast<const std::uint8_t *>(data);
        wrapped.insert(wrapped.end(), first, first + bytes);
        platform::Result<std::size_t> sent =
            inner_->SendTo(id, wrapped.data(), wrapped.size(), nat_);
        if (!sent.Ok()) {
            return sent.Code();
        }
        return bytes;
    }
    platform::Result<platform::Received> ReceiveFrom(platform::SocketId id, void *out,
                                                     std::size_t bytes) override {
        std::uint8_t buffer[2048];
        for (;;) {
            platform::Result<platform::Received> got =
                inner_->ReceiveFrom(id, buffer, sizeof buffer);
            if (!got.Ok()) {
                return got;
            }
            if (got.Value().from != nat_ || got.Value().bytes < kPrefix) {
                continue; // not through the NAT: a real network would not deliver it
            }
            std::size_t count = std::min(bytes, got.Value().bytes - kPrefix);
            std::memcpy(out, buffer + kPrefix, count);
            return platform::Received{count, getPrefix(buffer)};
        }
    }

    platform::Result<platform::SocketId> Open(platform::SocketKind kind) override {
        return inner_->Open(kind);
    }
    platform::Status Close(platform::SocketId id) override { return inner_->Close(id); }
    platform::Status SetOption(platform::SocketId id, platform::SocketOption option,
                               std::int32_t value) override {
        return inner_->SetOption(id, option, value);
    }
    platform::Result<std::int32_t> GetOption(platform::SocketId id,
                                             platform::SocketOption option) override {
        return inner_->GetOption(id, option);
    }
    platform::Status Bind(platform::SocketId id, platform::Endpoint local) override {
        return inner_->Bind(id, platform::Endpoint{kLoopback, local.port});
    }
    platform::Status Connect(platform::SocketId id, platform::Endpoint peer) override {
        return inner_->Connect(id, peer);
    }
    platform::Status Listen(platform::SocketId id, std::uint32_t backlog) override {
        return inner_->Listen(id, backlog);
    }
    platform::Result<platform::Accepted> Accept(platform::SocketId id) override {
        return inner_->Accept(id);
    }
    platform::Result<std::size_t> Send(platform::SocketId id, const void *data,
                                       std::size_t bytes) override {
        return inner_->Send(id, data, bytes);
    }
    platform::Result<std::size_t> Receive(platform::SocketId id, void *out,
                                          std::size_t bytes) override {
        return inner_->Receive(id, out, bytes);
    }
    platform::Status Shutdown(platform::SocketId id, platform::ShutdownHow how) override {
        return inner_->Shutdown(id, how);
    }
    platform::Result<platform::Endpoint> PeerName(platform::SocketId id) override {
        return inner_->PeerName(id);
    }
    platform::Result<platform::Endpoint> LocalName(platform::SocketId id) override {
        return inner_->LocalName(id);
    }
    platform::Result<std::size_t> Select(platform::SelectSet &sets,
                                         std::optional<std::uint32_t> timeoutMs) override {
        return inner_->Select(sets, timeoutMs);
    }
    platform::Result<std::vector<std::uint32_t>> Resolve(std::string_view name) override {
        return inner_->Resolve(name);
    }
    platform::Result<std::string> HostName() override { return inner_->HostName(); }
    platform::Result<platform::Rendezvous *> GetRendezvous() override {
        return platform::Error::kUnsupported;
    }

private:
    std::unique_ptr<platform::Transport> inner_;
    platform::Endpoint nat_;
};

inline platform::Result<platform::SocketId> openLoopback(platform::Transport &transport) {
    platform::Result<platform::SocketId> socket = transport.Open(platform::SocketKind::kUdp);
    if (!socket.Ok()) {
        return socket;
    }
    if (!transport.SetOption(socket.Value(), platform::SocketOption::kNonBlocking, 1).Ok() ||
        !transport.Bind(socket.Value(), platform::Endpoint{kLoopback, 0}).Ok()) {
        (void)transport.Close(socket.Value());
        return platform::Error::kIoError;
    }
    return socket;
}

// One simulated NAT on loopback: an inside port the client tunnels to, and one outside socket
// per mapping.
class NatForwarder {
public:
    NatForwarder(platform::Transport &transport, NatConfig config)
        : transport_(transport),
          model_(config, [this]() -> std::optional<std::uint16_t> { return openOutside(); }) {}

    bool Start() {
        platform::Result<platform::SocketId> inside = openLoopback(transport_);
        if (!inside.Ok()) {
            return false;
        }
        inside_ = inside.Value();
        platform::Result<platform::Endpoint> local = transport_.LocalName(inside_);
        insideEndpoint_ = local.ValueOr(platform::Endpoint{});
        return local.Ok();
    }

    platform::Endpoint InsideEndpoint() const { return insideEndpoint_; }

    void Sockets(std::vector<platform::SocketId> &out) const {
        out.push_back(inside_);
        for (const auto &[port, socket] : outside_) {
            (void)port;
            out.push_back(socket);
        }
    }

    void Poll() {
        std::uint8_t buffer[2048];
        for (;;) {
            platform::Result<platform::Received> got =
                transport_.ReceiveFrom(inside_, buffer, sizeof buffer);
            if (!got.Ok()) {
                break;
            }
            if (got.Value().bytes < kPrefix) {
                continue;
            }
            client_ = got.Value().from;
            const platform::Endpoint destination = getPrefix(buffer);
            const std::uint8_t *payload = buffer + kPrefix;
            const std::size_t bytes = got.Value().bytes - kPrefix;
            std::optional<platform::Endpoint> mapped =
                model_.Outbound(client_, destination, payload, bytes);
            if (mapped) {
                (void)transport_.SendTo(outside_[mapped->port], payload, bytes, destination);
            }
        }
        for (const auto &[port, socket] : outside_) {
            for (;;) {
                platform::Result<platform::Received> got =
                    transport_.ReceiveFrom(socket, buffer + kPrefix, sizeof buffer - kPrefix);
                if (!got.Ok()) {
                    break;
                }
                std::optional<platform::Endpoint> inside =
                    model_.Inbound(port, got.Value().from, buffer + kPrefix, got.Value().bytes);
                if (!inside) {
                    continue;
                }
                std::vector<std::uint8_t> wrapped;
                putPrefix(wrapped, got.Value().from);
                wrapped.insert(wrapped.end(), buffer + kPrefix,
                               buffer + kPrefix + got.Value().bytes);
                (void)transport_.SendTo(inside_, wrapped.data(), wrapped.size(), *inside);
            }
        }
    }

    std::uint32_t Dropped() const { return model_.Dropped(); }

private:
    platform::Transport &transport_;
    NatModel model_;
    platform::SocketId inside_;
    platform::Endpoint insideEndpoint_;
    platform::Endpoint client_;
    std::map<std::uint16_t, platform::SocketId> outside_;

    std::optional<std::uint16_t> openOutside() {
        platform::Result<platform::SocketId> socket = openLoopback(transport_);
        if (!socket.Ok()) {
            return std::nullopt;
        }
        platform::Result<platform::Endpoint> local = transport_.LocalName(socket.Value());
        if (!local.Ok()) {
            (void)transport_.Close(socket.Value());
            return std::nullopt;
        }
        outside_[local.Value().port] = socket.Value();
        return local.Value().port;
    }
};

using Clock = std::chrono::steady_clock;

inline std::uint32_t msSince(Clock::time_point start) {
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
}

constexpr std::uint64_t kSession = 0x5346434F50000002ull;
const std::vector<std::uint8_t> kHello = {0x48, 0x45, 0x4C, 0x4C, 0x4F};
const std::vector<std::uint8_t> kWelcome(2400, 0x57); // three envelopes

inline bool waitReadable(platform::Transport &transport, platform::SocketId socket,
                         std::uint32_t timeoutMs) {
    platform::SelectSet sets;
    sets.readable.push_back(socket);
    platform::Result<std::size_t> ready = transport.Select(sets, timeoutMs);
    return ready.Ok() && ready.Value() > 0;
}

// The host: binds, accepts the joiner, answers each hello with a welcome for 3 s. Returns the
// exit code.
inline int runHost(platform::Transport &transport, const net::Ticket &ticket,
                   std::uint64_t sessionId = kSession, std::uint32_t acceptMs = 10000) {
    platform::Rendezvous *rendezvous = transport.GetRendezvous().ValueOr(nullptr);
    if (!rendezvous) {
        return 10;
    }
    const std::string session = net::formatPeerName({sessionId, 1}).substr(0, 16);
    if (!rendezvous->Bind(session, 1, ticket.data(), ticket.size(), 2000).Ok()) {
        return 11;
    }
    platform::Result<platform::SocketId> socket = rendezvous->AcceptPeer(acceptMs);
    if (!socket.Ok()) {
        return 12;
    }
    const Clock::time_point start = Clock::now();
    bool greeted = false;
    while (msSince(start) < 3000) {
        if (!waitReadable(transport, socket.Value(), 100)) {
            continue;
        }
        std::uint8_t buffer[4096];
        platform::Result<std::size_t> got =
            transport.Receive(socket.Value(), buffer, sizeof buffer);
        if (!got.Ok()) {
            break;
        }
        if (std::vector<std::uint8_t>(buffer, buffer + got.Value()) == kHello) {
            greeted = true;
            (void)transport.Send(socket.Value(), kWelcome.data(), kWelcome.size());
        }
    }
    return greeted ? 0 : 13;
}

// The joiner: binds, reaches the host by name, sends hello until a welcome arrives. Writes its
// result line ("<path> <ms>", or "fail <why>") to `report`.
inline int runJoiner(platform::Transport &transport, const net::Ticket &ticket,
                     platform::Endpoint service, int report, std::uint64_t sessionId = kSession) {
    auto write = [&](const std::string &line) { (void)::write(report, line.data(), line.size()); };
    platform::Rendezvous *rendezvous = transport.GetRendezvous().ValueOr(nullptr);
    if (!rendezvous) {
        write("fail no rendezvous");
        return 20;
    }
    const std::string host = net::formatPeerName({sessionId, 1}).substr(0, 16);
    if (!rendezvous->Bind(host, 2, ticket.data(), ticket.size(), 2000).Ok()) {
        write("fail bind");
        return 21;
    }
    // The host binds at the same moment: until it has, the service answers kUnknownPeer.
    platform::Result<platform::SocketId> socket = platform::Error::kHostUnreachable;
    std::uint32_t connectMs = 0;
    for (int attempt = 0; attempt < 20 && !socket.Ok(); ++attempt) {
        const Clock::time_point start = Clock::now();
        socket = rendezvous->ConnectToPeer(host, 6000);
        connectMs = msSince(start);
        if (!socket.Ok() && socket.Code() == platform::Error::kHostUnreachable && connectMs < 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        break;
    }
    if (!socket.Ok()) {
        write(std::string("fail connect ") + platform::errorName(socket.Code()));
        return 22;
    }
    platform::Result<platform::Endpoint> peer = transport.PeerName(socket.Value());
    const char *path = peer.Ok() && peer.Value() == service ? "relay" : "direct";
    (void)transport.SetOption(socket.Value(), platform::SocketOption::kNonBlocking, 1);
    const Clock::time_point start = Clock::now();
    while (msSince(start) < 3000) {
        (void)transport.Send(socket.Value(), kHello.data(), kHello.size());
        if (!waitReadable(transport, socket.Value(), 200)) {
            continue;
        }
        std::uint8_t buffer[4096];
        platform::Result<std::size_t> got =
            transport.Receive(socket.Value(), buffer, sizeof buffer);
        if (got.Ok() && std::vector<std::uint8_t>(buffer, buffer + got.Value()) == kWelcome) {
            write(std::string(path) + " " + std::to_string(connectMs));
            return 0;
        }
    }
    write("fail exchange");
    return 23;
}

inline std::unique_ptr<platform::Transport>
clientTransport(platform::Endpoint nat, platform::Endpoint service, bool forceRelay) {
    net::RendezvousConfig config;
    config.serviceHost = "127.0.0.1";
    config.servicePort = service.port;
    config.forceRelay = forceRelay;
    config.localCandidates = false;
    return net::makeRendezvousTransport(
        std::make_unique<TunnelTransport>(platform::makeTransport(), nat), config);
}

} // namespace detail

// Runs one scenario: the parent serves and forwards until both children are done or 20 s pass.
inline PairResult runPair(Scenario scenario) {
    using namespace detail;
    PairResult result;
    std::unique_ptr<platform::Transport> transport = platform::makeTransport();
    platform::Result<platform::SocketId> serviceSocket = openLoopback(*transport);
    if (!serviceSocket.Ok()) {
        result.failure = "skipped: the host backend opens no UDP socket here";
        return result;
    }
    const platform::Endpoint service = transport->LocalName(serviceSocket.Value()).ValueOr({});

    NatConfig hostNat;
    hostNat.externalAddress = kLoopback;
    hostNat.dropPunches = scenario == Scenario::kBlocked;
    NatConfig joinerNat;
    joinerNat.externalAddress = kLoopback;
    joinerNat.kind = scenario == Scenario::kSymmetric ? NatKind::kSymmetric : NatKind::kCone;
    NatForwarder natA(*transport, hostNat);
    NatForwarder natB(*transport, joinerNat);
    if (!natA.Start() || !natB.Start()) {
        result.failure = "could not open the NAT sockets";
        return result;
    }

    ServiceKey key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::uint8_t>(0x30 + i);
    }
    const std::uint32_t unixNow = static_cast<std::uint32_t>(std::time(nullptr));
    const net::Ticket hostTicket = issueTicket(key, kSession, 1, unixNow + 600, 1);
    const net::Ticket joinerTicket = issueTicket(key, kSession, 2, unixNow + 600, 2);
    StandInService standIn(key);

    int report[2];
    if (::pipe(report) != 0) {
        result.failure = "no pipe";
        return result;
    }
    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t hostPid = ::fork();
    if (hostPid == 0) {
        ::close(report[0]);
        ::close(report[1]);
        int code = runHost(*clientTransport(natA.InsideEndpoint(), service, false), hostTicket);
        ::_exit(code);
    }
    const pid_t joinerPid = ::fork();
    if (joinerPid == 0) {
        ::close(report[0]);
        int code = runJoiner(
            *clientTransport(natB.InsideEndpoint(), service, scenario == Scenario::kForced),
            joinerTicket, service, report[1]);
        ::close(report[1]);
        ::_exit(code);
    }
    ::close(report[1]);
    if (hostPid < 0 || joinerPid < 0) {
        result.failure = "fork failed";
        return result;
    }

    const Clock::time_point start = Clock::now();
    bool hostDone = false;
    bool joinerDone = false;
    int hostStatus = 0;
    int joinerStatus = 0;
    std::uint8_t buffer[2048];
    while ((!hostDone || !joinerDone) && msSince(start) < 20000) {
        platform::SelectSet sets;
        sets.readable.push_back(serviceSocket.Value());
        natA.Sockets(sets.readable);
        natB.Sockets(sets.readable);
        (void)transport->Select(sets, 5);
        const std::uint32_t nowMs = msSince(start);
        std::vector<ServiceDatagram> out;
        for (;;) {
            platform::Result<platform::Received> got =
                transport->ReceiveFrom(serviceSocket.Value(), buffer, sizeof buffer);
            if (!got.Ok()) {
                break;
            }
            standIn.OnDatagram(got.Value().from, buffer, got.Value().bytes, nowMs,
                               static_cast<std::uint32_t>(std::time(nullptr)), out);
        }
        standIn.Tick(nowMs, out);
        for (const ServiceDatagram &datagram : out) {
            (void)transport->SendTo(serviceSocket.Value(), datagram.bytes.data(),
                                    datagram.bytes.size(), datagram.to);
        }
        natA.Poll();
        natB.Poll();
        if (!hostDone && ::waitpid(hostPid, &hostStatus, WNOHANG) == hostPid) {
            hostDone = true;
        }
        if (!joinerDone && ::waitpid(joinerPid, &joinerStatus, WNOHANG) == joinerPid) {
            joinerDone = true;
        }
    }
    if (!hostDone) {
        ::kill(hostPid, SIGKILL);
        ::waitpid(hostPid, &hostStatus, 0);
    }
    if (!joinerDone) {
        ::kill(joinerPid, SIGKILL);
        ::waitpid(joinerPid, &joinerStatus, 0);
    }

    char line[128] = {};
    ssize_t got = ::read(report[0], line, sizeof line - 1);
    ::close(report[0]);
    const std::string text = got > 0 ? std::string(line, static_cast<std::size_t>(got)) : "";
    const bool hostOk = hostDone && WIFEXITED(hostStatus) && WEXITSTATUS(hostStatus) == 0;
    if (text.rfind("fail", 0) == 0 || text.empty()) {
        result.failure = text.empty() ? "the joiner reported nothing" : text;
        return result;
    }
    std::size_t space = text.find(' ');
    result.path = text.substr(0, space);
    result.connectMs = static_cast<std::uint32_t>(std::stoul(text.substr(space + 1)));
    result.ok = hostOk;
    if (!hostOk) {
        result.failure = "the host exited with " +
                         std::to_string(WIFEXITED(hostStatus) ? WEXITSTATUS(hostStatus) : -1);
    }
    return result;
}

} // namespace sfcop::nettest

#endif // !defined(_WIN32)
