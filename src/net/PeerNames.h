// The names of RENDEZVOUS.md, "Names": a session is 16 lowercase hexadecimal digits, a peer is
// `<session>:<peer>` with 8 more, and `<session>` alone names the host, peer 1.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sfcop::net {

struct PeerName {
    std::uint64_t session = 0;
    std::uint32_t peer = 0;
};

constexpr std::uint32_t kHostPeer = 1;

// The session id in `text`, or nothing when it is not exactly 16 lowercase hexadecimal digits.
std::optional<std::uint64_t> parseSessionId(std::string_view text);

// The session and peer `text` names, or nothing when it has neither form, or names peer 0.
std::optional<PeerName> parsePeerName(std::string_view text);

// The `<session>:<peer>` form of a name.
std::string formatPeerName(PeerName name);

} // namespace sfcop::net
