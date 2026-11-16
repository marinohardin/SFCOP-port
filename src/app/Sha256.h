// SHA-256 (FIPS 180-4), fed in pieces so that a large file is hashed without being held in
// memory. Used to verify the install against the decompilation's
// analysis/results/retail_hashes.sha256.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace sfcop::app {

class Sha256 {
public:
    using Digest = std::array<std::uint8_t, 32>;

    Sha256();
    void update(const void *data, std::size_t size);
    // The digest of everything given so far. The object is spent afterwards.
    Digest finish();

    // 64 lowercase hex digits, as sha256sum prints them.
    static std::string toHex(const Digest &digest);

private:
    void compress(const std::uint8_t *block);

    std::array<std::uint32_t, 8> state_;
    std::array<std::uint8_t, 64> block_{};
    std::size_t blockUsed_ = 0;
    std::uint64_t totalBytes_ = 0;
};

} // namespace sfcop::app
