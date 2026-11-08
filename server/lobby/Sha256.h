// SHA-256 and HMAC-SHA-256 (FIPS 180-4, RFC 2104) for the ticket's mac (RENDEZVOUS.md, "Tickets").
// Written for this project; checked against the published test vectors in CryptoTest.cpp.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace sfcop::lobby {

using Digest = std::array<uint8_t, 32>;

Digest sha256(const uint8_t* data, size_t size);
Digest hmacSha256(const uint8_t* key, size_t keySize, const uint8_t* data, size_t size);

// Compares two byte ranges in time that depends only on the length (host secrets, macs).
bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t size);
bool constantTimeEqual(const std::string& a, const std::string& b);

} // namespace sfcop::lobby
