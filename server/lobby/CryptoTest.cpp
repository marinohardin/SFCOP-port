// SHA-256, HMAC-SHA-256 and base64url against published vectors (FIPS 180-4 examples, RFC 4231
// cases 1, 2 and 6, RFC 4648 section 10); the expected strings are copied from those documents.
#include "Base64Url.h"
#include "Sha256.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace sfcop::lobby;

namespace {
std::string hex(const Digest& d) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (uint8_t b : d) {
        s += digits[b >> 4];
        s += digits[b & 15];
    }
    return s;
}
const uint8_t* bytes(const std::string& s) { return reinterpret_cast<const uint8_t*>(s.data()); }
} // namespace

TEST_CASE("SHA-256 matches the FIPS 180-4 vectors") {
    CHECK(hex(sha256(nullptr, 0)) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    std::string abc = "abc";
    CHECK(hex(sha256(bytes(abc), abc.size())) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(hex(sha256(bytes(two), two.size())) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("SHA-256 handles the padding boundary at 55, 56 and 64 bytes") {
    // Lengths whose padding needs one block, two blocks, and an exact block; digests from sha256sum.
    CHECK(hex(sha256(bytes(std::string(55, 'a')), 55)) ==
          "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK(hex(sha256(bytes(std::string(56, 'a')), 56)) ==
          "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK(hex(sha256(bytes(std::string(64, 'a')), 64)) ==
          "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}

TEST_CASE("HMAC-SHA-256 matches RFC 4231") {
    std::vector<uint8_t> key1(20, 0x0b);
    std::string hi = "Hi There";
    CHECK(hex(hmacSha256(key1.data(), key1.size(), bytes(hi), hi.size())) ==
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

    std::string jefe = "Jefe", what = "what do ya want for nothing?";
    CHECK(hex(hmacSha256(bytes(jefe), jefe.size(), bytes(what), what.size())) ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    std::vector<uint8_t> longKey(131, 0xaa);
    std::string large = "Test Using Larger Than Block-Size Key - Hash Key First";
    CHECK(hex(hmacSha256(longKey.data(), longKey.size(), bytes(large), large.size())) ==
          "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

TEST_CASE("constant-time comparison tells equal from different") {
    CHECK(constantTimeEqual("abc", "abc"));
    CHECK_FALSE(constantTimeEqual("abc", "abd"));
    CHECK_FALSE(constantTimeEqual("abc", "abcd"));
}

TEST_CASE("base64url encodes the RFC 4648 examples without padding") {
    auto enc = [](const std::string& s) { return base64UrlEncode(bytes(s), s.size()); };
    CHECK(enc("") == "");
    CHECK(enc("f") == "Zg");
    CHECK(enc("fo") == "Zm8");
    CHECK(enc("foo") == "Zm9v");
    CHECK(enc("foob") == "Zm9vYg");
    const uint8_t fb[] = {0xFB, 0xFF};
    CHECK(base64UrlEncode(fb, 2) == "-_8");
}

TEST_CASE("base64url decodes what it encodes and refuses everything else") {
    auto d = base64UrlDecode("Zm9vYg");
    REQUIRE(d.has_value());
    CHECK(std::string(d->begin(), d->end()) == "foob");
    CHECK(base64UrlDecode("").has_value());
    CHECK_FALSE(base64UrlDecode("Zg==").has_value()); // padding
    CHECK_FALSE(base64UrlDecode("Zm9v+").has_value()); // outside the alphabet
    CHECK_FALSE(base64UrlDecode("Z").has_value());     // impossible length
    CHECK_FALSE(base64UrlDecode("Zh").has_value());    // unused bits not zero
}
