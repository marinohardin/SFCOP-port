// An in-memory IPv4 network for the in-process tests: hosts on the public side, hosts each
// behind a NAT of its own (NatModel.h), and one endpoint for the stand-in service. Every host gets
// a platform::Transport whose UDP sockets send into the network; a datagram is delivered at once,
// so a test advances time itself and pumps each machine. UDP only; TCP calls answer kUnsupported.
// Addresses are from the RFC 5737 ranges.
#pragma once

#include "NatModel.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sfcop::nettest {

struct SimDatagram {
    platform::Endpoint from;
    std::vector<std::uint8_t> bytes;
};

class SimNetwork {
public:
    // Return true to lose the datagram (a test's packet loss).
    using Filter = std::function<bool(platform::Endpoint from, platform::Endpoint to,
                                      const std::vector<std::uint8_t> &bytes)>;

    int AddPublicHost(std::uint32_t address) { return add(address, std::nullopt); }
    int AddNattedHost(std::uint32_t address, NatConfig nat) { return add(address, nat); }

    void SetService(platform::Endpoint service) { service_ = service; }
    std::deque<SimDatagram> &ServiceInbox() { return serviceInbox_; }
    void SendFromService(platform::Endpoint to, const std::vector<std::uint8_t> &bytes) {
        deliverPublic(service_, to, bytes);
    }
    void SetFilter(Filter filter) { filter_ = std::move(filter); }
    NatModel &Nat(int host) { return *hosts_[host].nat; }

    std::unique_ptr<platform::Transport> MakeTransport(int host);

    // Internal to SimTransport.
    std::uint16_t Allocate(int host) { return hosts_[host].nextPort++; }
    std::deque<SimDatagram> &Inbox(int host, std::uint16_t port) {
        return hosts_[host].sockets[port];
    }
    void ReleasePort(int host, std::uint16_t port) { hosts_[host].sockets.erase(port); }
    std::uint32_t Address(int host) const { return hosts_[host].address; }
    void Send(int host, platform::Endpoint from, platform::Endpoint to,
              const std::vector<std::uint8_t> &bytes) {
        Host &source = hosts_[host];
        if (to.address == source.address) {
            deliver(host, from, to.port, bytes);
            return;
        }
        platform::Endpoint outside = from;
        if (source.nat) {
            std::optional<platform::Endpoint> mapped =
                source.nat->Outbound(from, to, bytes.data(), bytes.size());
            if (!mapped) {
                return;
            }
            outside = *mapped;
        }
        deliverPublic(outside, to, bytes);
    }

private:
    struct Host {
        std::uint32_t address = 0;
        std::optional<NatModel> nat;
        std::map<std::uint16_t, std::deque<SimDatagram>> sockets;
        std::uint16_t nextPort = 50000;
    };

    std::vector<Host> hosts_;
    platform::Endpoint service_;
    std::deque<SimDatagram> serviceInbox_;
    Filter filter_;

    int add(std::uint32_t address, std::optional<NatConfig> nat) {
        Host host;
        host.address = address;
        if (nat) {
            host.nat.emplace(*nat);
        }
        hosts_.push_back(std::move(host));
        return static_cast<int>(hosts_.size() - 1);
    }

    void deliver(int host, platform::Endpoint from, std::uint16_t port,
                 const std::vector<std::uint8_t> &bytes) {
        auto socket = hosts_[host].sockets.find(port);
        if (socket != hosts_[host].sockets.end()) {
            socket->second.push_back(SimDatagram{from, bytes});
        }
    }

    void deliverPublic(platform::Endpoint from, platform::Endpoint to,
                       const std::vector<std::uint8_t> &bytes) {
        if (filter_ && filter_(from, to, bytes)) {
            return;
        }
        if (to == service_) {
            serviceInbox_.push_back(SimDatagram{from, bytes});
            return;
        }
        for (std::size_t i = 0; i < hosts_.size(); ++i) {
            Host &host = hosts_[i];
            if (!host.nat && host.address == to.address) {
                deliver(static_cast<int>(i), from, to.port, bytes);
                return;
            }
            if (host.nat && host.nat->ExternalAddress() == to.address) {
                std::optional<platform::Endpoint> inside =
                    host.nat->Inbound(to.port, from, bytes.data(), bytes.size());
                if (inside) {
                    deliver(static_cast<int>(i), from, inside->port, bytes);
                }
                return;
            }
        }
    }
};

// A Transport on one host of a SimNetwork: UDP sockets only, never blocking.
class SimTransport final : public platform::Transport {
public:
    SimTransport(SimNetwork &network, int host) : network_(network), host_(host) {}

    platform::Result<platform::SocketId> Open(platform::SocketKind kind) override {
        if (kind != platform::SocketKind::kUdp) {
            return platform::Error::kUnsupported;
        }
        std::uint32_t id = next_++;
        ports_[id] = 0;
        return platform::SocketId{id};
    }
    platform::Status Close(platform::SocketId id) override {
        auto found = ports_.find(id.value);
        if (found == ports_.end()) {
            return platform::Error::kInvalidHandle;
        }
        if (found->second != 0) {
            network_.ReleasePort(host_, found->second);
        }
        ports_.erase(found);
        return {};
    }
    platform::Status SetOption(platform::SocketId id, platform::SocketOption,
                               std::int32_t) override {
        return ports_.count(id.value) ? platform::Status{}
                                      : platform::Status{platform::Error::kInvalidHandle};
    }
    platform::Result<std::int32_t> GetOption(platform::SocketId, platform::SocketOption) override {
        return platform::Error::kUnsupported;
    }
    platform::Status Bind(platform::SocketId id, platform::Endpoint local) override {
        auto found = ports_.find(id.value);
        if (found == ports_.end()) {
            return platform::Error::kInvalidHandle;
        }
        if (found->second != 0) {
            return platform::Error::kWrongState;
        }
        found->second = local.port != 0 ? local.port : network_.Allocate(host_);
        (void)network_.Inbox(host_, found->second);
        return {};
    }
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
    platform::Result<std::size_t> SendTo(platform::SocketId id, const void *data, std::size_t bytes,
                                         platform::Endpoint peer) override {
        auto found = ports_.find(id.value);
        if (found == ports_.end()) {
            return platform::Error::kInvalidHandle;
        }
        if (found->second == 0) {
            found->second = network_.Allocate(host_);
            (void)network_.Inbox(host_, found->second);
        }
        const auto *first = static_cast<const std::uint8_t *>(data);
        network_.Send(host_, platform::Endpoint{network_.Address(host_), found->second}, peer,
                      std::vector<std::uint8_t>(first, first + bytes));
        return bytes;
    }
    platform::Result<platform::Received> ReceiveFrom(platform::SocketId id, void *out,
                                                     std::size_t bytes) override {
        auto found = ports_.find(id.value);
        if (found == ports_.end()) {
            return platform::Error::kInvalidHandle;
        }
        if (found->second == 0) {
            return platform::Error::kWrongState;
        }
        std::deque<SimDatagram> &inbox = network_.Inbox(host_, found->second);
        if (inbox.empty()) {
            return platform::Error::kWouldBlock;
        }
        SimDatagram datagram = std::move(inbox.front());
        inbox.pop_front();
        std::size_t count = std::min(bytes, datagram.bytes.size());
        std::copy(datagram.bytes.begin(), datagram.bytes.begin() + count,
                  static_cast<std::uint8_t *>(out));
        return platform::Received{count, datagram.from};
    }
    platform::Status Shutdown(platform::SocketId, platform::ShutdownHow) override {
        return platform::Error::kUnsupported;
    }
    platform::Result<platform::Endpoint> PeerName(platform::SocketId) override {
        return platform::Error::kUnsupported;
    }
    platform::Result<platform::Endpoint> LocalName(platform::SocketId id) override {
        auto found = ports_.find(id.value);
        if (found == ports_.end()) {
            return platform::Error::kInvalidHandle;
        }
        if (found->second == 0) {
            return platform::Error::kWrongState;
        }
        return platform::Endpoint{network_.Address(host_), found->second};
    }
    platform::Result<std::size_t> Select(platform::SelectSet &sets,
                                         std::optional<std::uint32_t>) override {
        std::vector<platform::SocketId> readable;
        for (platform::SocketId id : sets.readable) {
            auto found = ports_.find(id.value);
            if (found == ports_.end()) {
                return platform::Error::kInvalidHandle;
            }
            if (found->second != 0 && !network_.Inbox(host_, found->second).empty()) {
                readable.push_back(id);
            }
        }
        sets.readable = readable;
        sets.errored.clear();
        return sets.readable.size() + sets.writable.size();
    }
    platform::Result<std::vector<std::uint32_t>> Resolve(std::string_view name) override {
        if (name == HostName().Value()) {
            return std::vector<std::uint32_t>{network_.Address(host_)};
        }
        return platform::Error::kNotFound;
    }
    platform::Result<std::string> HostName() override {
        return std::string("simhost") + std::to_string(host_);
    }
    platform::Result<platform::Rendezvous *> GetRendezvous() override {
        return platform::Error::kUnsupported;
    }

private:
    SimNetwork &network_;
    int host_;
    std::uint32_t next_ = 1;
    std::map<std::uint32_t, std::uint16_t> ports_;
};

inline std::unique_ptr<platform::Transport> SimNetwork::MakeTransport(int host) {
    return std::make_unique<SimTransport>(*this, host);
}

} // namespace sfcop::nettest
