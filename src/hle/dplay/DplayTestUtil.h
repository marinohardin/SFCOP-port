// Test helpers for the DirectPlay module (tests only): a clock that moves only when waited on,
// an in-memory UDP network of several machines, and hand-written byte strings.
#pragma once

#include "hle/dplay/Address.h"
#include "hle/dplay/DirectPlay.h"
#include "platform/Clock/Clock.h"
#include "platform/Transport/Transport.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sfcop::hle::dplay::test {

// "53 46 44 50" -> {0x53, 0x46, 0x44, 0x50}; spaces and newlines are ignored.
inline std::vector<std::uint8_t> hex(const std::string &text) {
    std::vector<std::uint8_t> out;
    std::string digits;
    for (char c : text) {
        if (c != ' ' && c != '\n') {
            digits.push_back(c);
        }
        if (digits.size() == 2) {
            out.push_back(static_cast<std::uint8_t>(std::strtoul(digits.c_str(), nullptr, 16)));
            digits.clear();
        }
    }
    return out;
}

inline std::vector<std::uint8_t> concat(std::initializer_list<std::vector<std::uint8_t>> parts) {
    std::vector<std::uint8_t> out;
    for (const auto &p : parts) {
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

inline std::vector<std::uint8_t> guidBytes(const Guid &g) {
    return std::vector<std::uint8_t>(g.bytes.begin(), g.bytes.end());
}

// The application GUID the tests use: {11223344-5566-7788-99AA-BBCCDDEEFF00}.
inline Guid testApplication() {
    return makeGuid(0x11223344, 0x5566, 0x7788, {0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00});
}

// A clock whose time moves only through Sleep() (and Advance()); Sleep() then runs `onSleep`.
class FakeClock final : public platform::Clock {
public:
    std::uint32_t now = 1000;
    std::function<void()> onSleep;

    void Advance(std::uint32_t ms) { now += ms; }

    platform::Result<std::uint32_t> Ticks() override { return now; }
    platform::Result<std::uint64_t> Counter() override { return std::uint64_t{now}; }
    platform::Result<std::uint64_t> CounterFrequency() override { return std::uint64_t{1000}; }
    platform::Result<platform::CivilTime> Now(platform::TimeZone) override {
        return platform::Error::kUnsupported;
    }
    platform::Result<std::int64_t> UnixTime() override { return platform::Error::kUnsupported; }
    platform::Status Sleep(std::uint32_t ms) override {
        now += ms;
        if (onSleep) {
            onSleep();
        }
        return {};
    }
};

class FakeNet;

// One machine's sockets on a FakeNet. Select() never waits.
class FakeTransport final : public platform::Transport {
public:
    FakeTransport(FakeNet &net, std::uint32_t address) : net_(net), address_(address) {}

    struct Datagram {
        std::vector<std::uint8_t> bytes;
        platform::Endpoint from;
    };
    struct Socket {
        std::uint16_t port = 0;
        bool bound = false;
        std::deque<Datagram> queue;
    };

    std::uint32_t Address() const { return address_; }
    std::map<std::uint32_t, Socket> sockets;

    platform::Result<platform::SocketId> Open(platform::SocketKind kind) override {
        if (kind != platform::SocketKind::kUdp) {
            return platform::Error::kUnsupported;
        }
        std::uint32_t id = nextSocket_++;
        sockets[id] = Socket{};
        return platform::SocketId{id};
    }
    platform::Status Close(platform::SocketId s) override {
        return sockets.erase(s.value) ? platform::Status{} : platform::Error::kInvalidHandle;
    }
    platform::Status SetOption(platform::SocketId s, platform::SocketOption,
                               std::int32_t) override {
        return sockets.count(s.value) ? platform::Status{} : platform::Error::kInvalidHandle;
    }
    platform::Result<std::int32_t> GetOption(platform::SocketId, platform::SocketOption) override {
        return std::int32_t{0};
    }
    platform::Status Bind(platform::SocketId s, platform::Endpoint local) override;
    platform::Status Connect(platform::SocketId, platform::Endpoint) override {
        return platform::Error::kUnsupported;
    }
    platform::Status Listen(platform::SocketId, std::uint32_t) override {
        return platform::Error::kUnsupported;
    }
    platform::Result<platform::Accepted> Accept(platform::SocketId) override {
        return platform::Error::kUnsupported;
    }
    platform::Result<std::size_t> Send(platform::SocketId, const void *, std::size_t) override {
        return platform::Error::kUnsupported;
    }
    platform::Result<std::size_t> Receive(platform::SocketId, void *, std::size_t) override {
        return platform::Error::kUnsupported;
    }
    platform::Result<std::size_t> SendTo(platform::SocketId s, const void *data, std::size_t bytes,
                                         platform::Endpoint peer) override;
    platform::Result<platform::Received> ReceiveFrom(platform::SocketId s, void *out,
                                                     std::size_t bytes) override {
        auto it = sockets.find(s.value);
        if (it == sockets.end()) {
            return platform::Error::kInvalidHandle;
        }
        if (it->second.queue.empty()) {
            return platform::Error::kWouldBlock;
        }
        Datagram d = it->second.queue.front();
        it->second.queue.pop_front();
        std::size_t n = d.bytes.size() < bytes ? d.bytes.size() : bytes;
        if (n != 0) {
            std::memcpy(out, d.bytes.data(), n);
        }
        return platform::Received{n, d.from};
    }
    platform::Status Shutdown(platform::SocketId, platform::ShutdownHow) override {
        return platform::Error::kUnsupported;
    }
    platform::Result<platform::Endpoint> PeerName(platform::SocketId) override {
        return platform::Error::kNotConnected;
    }
    platform::Result<platform::Endpoint> LocalName(platform::SocketId s) override {
        auto it = sockets.find(s.value);
        if (it == sockets.end()) {
            return platform::Error::kInvalidHandle;
        }
        return platform::Endpoint{address_, it->second.port};
    }
    platform::Result<std::size_t> Select(platform::SelectSet &set,
                                         std::optional<std::uint32_t>) override {
        std::vector<platform::SocketId> ready;
        for (platform::SocketId s : set.readable) {
            auto it = sockets.find(s.value);
            if (it != sockets.end() && !it->second.queue.empty()) {
                ready.push_back(s);
            }
        }
        set.readable = ready;
        set.writable.clear();
        set.errored.clear();
        return ready.size();
    }
    platform::Result<std::vector<std::uint32_t>> Resolve(std::string_view) override {
        return platform::Error::kNotFound;
    }
    platform::Result<std::string> HostName() override { return std::string("testhost"); }
    platform::Result<platform::Rendezvous *> GetRendezvous() override {
        return platform::Error::kUnsupported;
    }

    // Puts a datagram on the socket bound to `port`, if any.
    void Deliver(std::uint16_t port, const std::vector<std::uint8_t> &bytes,
                 platform::Endpoint from) {
        for (auto &entry : sockets) {
            if (entry.second.bound && entry.second.port == port) {
                entry.second.queue.push_back(Datagram{bytes, from});
                return;
            }
        }
    }

private:
    FakeNet &net_;
    std::uint32_t address_;
    std::uint32_t nextSocket_ = 1;
    std::uint16_t nextPort_ = 50000;
    friend class FakeNet;
};

// Several machines on one segment. A datagram to 255.255.255.255 reaches every machine's
// socket on that port; to 127.0.0.1 only the sender's own. `drop` may lose any datagram.
class FakeNet {
public:
    std::function<bool(platform::Endpoint from, platform::Endpoint to,
                       const std::vector<std::uint8_t> &bytes)>
        drop;
    std::size_t datagrams = 0;

    FakeTransport &Add(std::uint32_t address) {
        machines_.push_back(std::make_unique<FakeTransport>(*this, address));
        return *machines_.back();
    }

    void Send(FakeTransport &sender, std::uint16_t fromPort, platform::Endpoint to,
              const std::vector<std::uint8_t> &bytes) {
        ++datagrams;
        if (to.address == 0x7F000001) {
            platform::Endpoint from{0x7F000001, fromPort};
            if (!drop || !drop(from, to, bytes)) {
                sender.Deliver(to.port, bytes, from);
            }
            return;
        }
        platform::Endpoint from{sender.Address(), fromPort};
        for (auto &m : machines_) {
            if (to.address == 0xFFFFFFFF || m->Address() == to.address) {
                if (!drop || !drop(from, to, bytes)) {
                    m->Deliver(to.port, bytes, from);
                }
            }
        }
    }

private:
    std::vector<std::unique_ptr<FakeTransport>> machines_;
};

inline platform::Status FakeTransport::Bind(platform::SocketId s, platform::Endpoint local) {
    auto it = sockets.find(s.value);
    if (it == sockets.end()) {
        return platform::Error::kInvalidHandle;
    }
    if (it->second.bound) {
        return platform::Error::kWrongState;
    }
    std::uint16_t port = local.port != 0 ? local.port : nextPort_++;
    for (const auto &entry : sockets) {
        if (entry.second.bound && entry.second.port == port) {
            return platform::Error::kAddressInUse;
        }
    }
    it->second.port = port;
    it->second.bound = true;
    return {};
}

inline platform::Result<std::size_t> FakeTransport::SendTo(platform::SocketId s, const void *data,
                                                           std::size_t bytes,
                                                           platform::Endpoint peer) {
    auto it = sockets.find(s.value);
    if (it == sockets.end()) {
        return platform::Error::kInvalidHandle;
    }
    if (!it->second.bound) {
        (void)Bind(s, platform::Endpoint{});
    }
    const auto *p = static_cast<const std::uint8_t *>(data);
    net_.Send(*this, it->second.port, peer, std::vector<std::uint8_t>(p, p + bytes));
    return bytes;
}

// A machine on a FakeNet with its DirectPlay object.
struct Node {
    FakeTransport *transport = nullptr;
    std::unique_ptr<DirectPlay> dp;
    std::vector<std::uint32_t> signalled;
};

// Machines 192.0.2.1, .2, ... on one FakeNet and one FakeClock. While any object waits, the
// clock moves and every machine is serviced.
class Lan {
public:
    FakeNet net;
    FakeClock clock;
    std::vector<std::unique_ptr<Node>> nodes;

    explicit Lan(std::size_t machines, Config config = testConfig()) {
        for (std::size_t i = 0; i < machines; ++i) {
            auto node = std::make_unique<Node>();
            node->transport = &net.Add(0xC0000201 + static_cast<std::uint32_t>(i));
            Environment env;
            env.transport = node->transport;
            env.clock = &clock;
            Node *raw = node.get();
            env.signal = [raw](std::uint32_t event) { raw->signalled.push_back(event); };
            Config c = config;
            c.seed = config.seed + i;
            node->dp = std::make_unique<DirectPlay>(env, c);
            nodes.push_back(std::move(node));
        }
        clock.onSleep = [this] { ServiceAll(); };
    }

    static Config testConfig() {
        Config c;
        c.searchLoopback = false;
        return c;
    }

    DirectPlay &operator[](std::size_t i) { return *nodes[i]->dp; }

    void ServiceAll() {
        if (servicing_) {
            return;
        }
        servicing_ = true;
        for (auto &n : nodes) {
            if (n->dp) {
                n->dp->Service(0);
            }
        }
        servicing_ = false;
    }

    // Lets `ms` pass in steps of 10, servicing every machine at each.
    void Run(std::uint32_t ms) {
        for (std::uint32_t t = 0; t < ms; t += 10) {
            clock.Advance(10);
            ServiceAll();
        }
    }

private:
    bool servicing_ = false;
};

// InitializeConnection with the plain TCP/IP connection.
inline HResult initialize(DirectPlay &dp) {
    std::vector<std::uint8_t> connection = tcpIpConnection();
    return dp.InitializeConnection(connection.data(), connection.size(), 0);
}

// Hosts a session of the test application on `dp`.
inline HResult host(DirectPlay &dp, const std::string &name,
                    std::uint32_t flags = kSessionKeepAlive | kSessionMigrateHost,
                    std::uint32_t maxPlayers = 0,
                    const std::optional<std::string> &password = std::nullopt) {
    HResult hr = initialize(dp);
    if (hr != kDpOk) {
        return hr;
    }
    SessionDesc d;
    d.flags = flags;
    d.application = testApplication();
    d.maxPlayers = maxPlayers;
    d.name = name;
    d.password = password;
    return dp.Open(d, kOpenCreate);
}

// Enumerates and joins the first session found, with `password`.
inline HResult join(DirectPlay &dp, const std::optional<std::string> &password = std::nullopt) {
    HResult hr = initialize(dp);
    if (hr != kDpOk) {
        return hr;
    }
    SessionDesc filter;
    filter.application = testApplication();
    std::optional<SessionDesc> first;
    hr = dp.EnumSessions(
        filter, 100,
        [&](const SessionDesc *s, std::uint32_t &, std::uint32_t) {
            if (s != nullptr && !first) {
                first = *s;
            }
            return false;
        },
        kEnumSessionsAll);
    if (hr != kDpOk) {
        return hr;
    }
    if (!first) {
        return kDpErrNoSessions;
    }
    SessionDesc d;
    d.instance = first->instance;
    d.password = password;
    return dp.Open(d, kOpenJoin);
}

// Takes every message waiting on `dp`.
inline std::vector<ReceivedMessage> drain(DirectPlay &dp) {
    std::vector<ReceivedMessage> out;
    for (;;) {
        Dpid from = 0, to = 0;
        std::uint32_t size = 0xFFFFFFFF;
        ReceivedMessage m;
        if (dp.Receive(&from, &to, kReceiveAll, &size, &m) != kDpOk) {
            return out;
        }
        out.push_back(m);
    }
}

// The dwType of a system message.
inline std::uint32_t typeOf(const ReceivedMessage &m) {
    const auto &b = m.blob.bytes;
    return b.size() < 4 ? 0 : b[0] | b[1] << 8 | b[2] << 16 | static_cast<std::uint32_t>(b[3]) << 24;
}

} // namespace sfcop::hle::dplay::test
