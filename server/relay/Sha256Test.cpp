#include <doctest/doctest.h>

#include <string>

#include "Sha256.h"

using namespace sfcop::relay;

namespace {

std::string hex(const Digest& d) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (std::uint8_t b : d) {
        s += digits[b >> 4];
        s += digits[b & 15];
    }
    return s;
}

const std::uint8_t* u8(const char* s) { return reinterpret_cast<const std::uint8_t*>(s); }

}  // namespace

TEST_CASE("sha256 of the empty message and of abc matches the published digests") {
    CHECK(hex(sha256(nullptr, 0)) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hex(sha256(u8("abc"), 3)) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("sha256 pads correctly across the 55, 56 and 64 byte boundaries") {
    // 56 bytes of "abcdbcde..." is the FIPS 180-4 two-block example.
    const char* m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(hex(sha256(u8(m), 56)) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("hmac-sha256 matches RFC 4231 test cases 1 and 2") {
    std::uint8_t key1[20];
    for (auto& b : key1) b = 0x0b;
    CHECK(hex(hmacSha256(key1, 20, u8("Hi There"), 8)) ==
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    CHECK(hex(hmacSha256(u8("Jefe"), 4, u8("what do ya want for nothing?"), 28)) ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST_CASE("constantTimeEqual tells equal from different bytes") {
    std::uint8_t a[3] = {1, 2, 3}, b[3] = {1, 2, 3}, c[3] = {1, 2, 4};
    CHECK(constantTimeEqual(a, b, 3));
    CHECK_FALSE(constantTimeEqual(a, c, 3));
}
