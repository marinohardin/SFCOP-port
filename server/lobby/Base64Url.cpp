#include "Base64Url.h"

namespace sfcop::lobby {

namespace {
constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int valueOf(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}
} // namespace

std::string base64UrlEncode(const uint8_t* data, size_t size) {
    std::string out;
    out.reserve((size * 4 + 2) / 3);
    size_t i = 0;
    for (; i + 3 <= size; i += 3) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out += kAlphabet[v >> 18];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
    }
    if (size - i == 1) {
        uint32_t v = uint32_t(data[i]) << 16;
        out += kAlphabet[v >> 18];
        out += kAlphabet[(v >> 12) & 63];
    } else if (size - i == 2) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out += kAlphabet[v >> 18];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
    }
    return out;
}

std::optional<std::vector<uint8_t>> base64UrlDecode(const std::string& text) {
    if (text.size() % 4 == 1) return std::nullopt;
    std::vector<uint8_t> out;
    out.reserve(text.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : text) {
        int v = valueOf(c);
        if (v < 0) return std::nullopt;
        acc = (acc << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(uint8_t(acc >> bits));
            acc &= (1u << bits) - 1;
        }
    }
    if (acc != 0) return std::nullopt; // unused bits must be zero
    return out;
}

} // namespace sfcop::lobby
