#include "net/PeerNames.h"

namespace sfcop::net {
namespace {

std::optional<std::uint64_t> parseHex(std::string_view text, std::size_t digits) {
    if (text.size() != digits) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (char c : text) {
        int digit;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else {
            return std::nullopt;
        }
        value = (value << 4) | static_cast<std::uint64_t>(digit);
    }
    return value;
}

void appendHex(std::string &out, std::uint64_t value, int digits) {
    static const char kDigits[] = "0123456789abcdef";
    for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4) {
        out.push_back(kDigits[(value >> shift) & 0xF]);
    }
}

} // namespace

std::optional<std::uint64_t> parseSessionId(std::string_view text) {
    return parseHex(text, 16);
}

std::optional<PeerName> parsePeerName(std::string_view text) {
    std::optional<std::uint64_t> session = parseSessionId(text.substr(0, 16));
    if (!session) {
        return std::nullopt;
    }
    if (text.size() == 16) {
        return PeerName{*session, kHostPeer};
    }
    if (text.size() != 25 || text[16] != ':') {
        return std::nullopt;
    }
    std::optional<std::uint64_t> peer = parseHex(text.substr(17), 8);
    if (!peer || *peer == 0) {
        return std::nullopt;
    }
    return PeerName{*session, static_cast<std::uint32_t>(*peer)};
}

std::string formatPeerName(PeerName name) {
    std::string out;
    appendHex(out, name.session, 16);
    out.push_back(':');
    appendHex(out, name.peer, 8);
    return out;
}

} // namespace sfcop::net
