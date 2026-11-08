// SHA-256 and HMAC-SHA-256 (FIPS 180-4, RFC 2104), for checking the lobby's tickets. The relay
// takes no dependency for this (CONVENTIONS §5).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sfcop::relay {

using Digest = std::array<std::uint8_t, 32>;

Digest sha256(const std::uint8_t* data, std::size_t size);
Digest hmacSha256(const std::uint8_t* key, std::size_t keySize, const std::uint8_t* data,
                  std::size_t size);

// Compares in time that does not depend on where the first difference is.
bool constantTimeEqual(const std::uint8_t* a, const std::uint8_t* b, std::size_t size);

}  // namespace sfcop::relay
