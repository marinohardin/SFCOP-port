// Unpadded base64url (RFC 4648 section 5): the text form of tickets and host secrets
// (LOBBY_API.md, "Ids").
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sfcop::lobby {

std::string base64UrlEncode(const uint8_t* data, size_t size);
// Strict: no padding, no characters outside the alphabet, and the unused bits of the last
// character zero; anything else is nullopt.
std::optional<std::vector<uint8_t>> base64UrlDecode(const std::string& text);

} // namespace sfcop::lobby
