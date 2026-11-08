// The address translation of one simulated NAT, for the in-process tests (SimNetwork.h) and the
// two-process harness's UDP forwarder (NetPair.h). Filtering is address-and-port dependent in
// both kinds: a datagram from outside passes only from an address and port the inside already
// sent to through that mapping. The mapping is either
//
// - cone (endpoint-independent): one external port per inside socket, whatever the destination;
//   two such NATs punch directly; or
// - symmetric: a new external port for each destination, so the port the rendezvous service saw
//   is not the one a peer sees, and a punch through it fails.
//
// It can also be told to drop the punch itself (every Probe and ProbeReply, both ways), which is
// how the harness blocks traversal on a network that would allow it.
#pragma once

#include "net/RendezvousMessages.h"
#include "platform/Transport/Transport.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>

namespace sfcop::nettest {

enum class NatKind : std::uint8_t { kCone, kSymmetric };

struct NatConfig {
    NatKind kind = NatKind::kCone;
    bool dropPunches = false;
    std::uint32_t externalAddress = 0;
};

// True for the rendezvous messages of the punch itself (Probe, ProbeReply).
inline bool isPunch(const std::uint8_t *data, std::size_t bytes) {
    return bytes >= 6 && data[0] == 0x53 && data[1] == 0x46 && data[2] == 0x52 && data[3] == 0x56 &&
           (data[5] == static_cast<std::uint8_t>(net::RvType::kProbe) ||
            data[5] == static_cast<std::uint8_t>(net::RvType::kProbeReply));
}

class NatModel {
public:
    using PortSource = std::function<std::optional<std::uint16_t>()>;

    // `ports` gives each new mapping its external port (the forwarder opens a socket for it);
    // by default they are numbered from 40000.
    explicit NatModel(NatConfig config, PortSource ports = {})
        : config_(config), ports_(std::move(ports)) {}

    // A datagram from `inside` to `destination`: the external endpoint it leaves from, or
    // nothing when the NAT drops it.
    std::optional<platform::Endpoint> Outbound(platform::Endpoint inside,
                                               platform::Endpoint destination,
                                               const std::uint8_t *data, std::size_t bytes) {
        if (config_.dropPunches && isPunch(data, bytes)) {
            ++dropped_;
            return std::nullopt;
        }
        MappingKey key{inside.address, inside.port, 0, 0};
        if (config_.kind == NatKind::kSymmetric) {
            key = MappingKey{inside.address, inside.port, destination.address, destination.port};
        }
        auto found = mappings_.find(key);
        if (found == mappings_.end()) {
            std::optional<std::uint16_t> port = ports_ ? ports_() : nextPort();
            if (!port) {
                ++dropped_;
                return std::nullopt;
            }
            found = mappings_.emplace(key, *port).first;
            inside_[*port] = inside;
        }
        const std::uint16_t external = found->second;
        allowed_.insert({external, destination.address, destination.port});
        return platform::Endpoint{config_.externalAddress, external};
    }

    // A datagram from `source` to the external `port`: the inside endpoint it goes to, or
    // nothing when the NAT drops it.
    std::optional<platform::Endpoint> Inbound(std::uint16_t port, platform::Endpoint source,
                                              const std::uint8_t *data, std::size_t bytes) {
        auto inside = inside_.find(port);
        if (inside == inside_.end() || allowed_.count({port, source.address, source.port}) == 0 ||
            (config_.dropPunches && isPunch(data, bytes))) {
            ++dropped_;
            return std::nullopt;
        }
        return inside->second;
    }

    std::uint32_t ExternalAddress() const { return config_.externalAddress; }
    std::size_t Mappings() const { return mappings_.size(); }
    std::uint32_t Dropped() const { return dropped_; }

private:
    using MappingKey = std::tuple<std::uint32_t, std::uint16_t, std::uint32_t, std::uint16_t>;
    using Permission = std::tuple<std::uint16_t, std::uint32_t, std::uint16_t>;

    NatConfig config_;
    PortSource ports_;
    std::uint16_t next_ = 40000;
    std::map<MappingKey, std::uint16_t> mappings_;
    std::map<std::uint16_t, platform::Endpoint> inside_;
    std::set<Permission> allowed_;
    std::uint32_t dropped_ = 0;

    std::optional<std::uint16_t> nextPort() { return next_++; }
};

} // namespace sfcop::nettest
