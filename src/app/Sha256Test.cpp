#include "app/Sha256.h"

#include <cstring>
#include <string>

#include <doctest/doctest.h>

using sfcop::app::Sha256;

namespace {

std::string hashOf(const std::string &text) {
    Sha256 hash;
    hash.update(text.data(), text.size());
    return Sha256::toHex(hash.finish());
}

// The two-block message of FIPS 180-4's examples.
const char kTwoBlocks[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
const char kTwoBlocksDigest[] = "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";

} // namespace

TEST_CASE("SHA-256 of the FIPS 180-4 example messages") {
    CHECK(hashOf("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hashOf("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hashOf(kTwoBlocks) == kTwoBlocksDigest);
}

TEST_CASE("SHA-256 of a million 'a' fed in uneven pieces") {
    Sha256 hash;
    const std::string piece(997, 'a');
    std::size_t left = 1000000;
    while (left > 0) {
        const std::size_t take = left < piece.size() ? left : piece.size();
        hash.update(piece.data(), take);
        left -= take;
    }
    CHECK(Sha256::toHex(hash.finish()) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("SHA-256 gives the same digest wherever the input is split") {
    const std::size_t length = std::strlen(kTwoBlocks);
    for (std::size_t split = 0; split <= length; ++split) {
        CAPTURE(split);
        Sha256 hash;
        hash.update(kTwoBlocks, split);
        hash.update(kTwoBlocks + split, length - split);
        CHECK(Sha256::toHex(hash.finish()) == kTwoBlocksDigest);
    }
}

TEST_CASE("SHA-256 pads messages whose length ends at the 55, 56 and 64 byte edges") {
    CHECK(hashOf(std::string(55, 'a')) ==
          "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK(hashOf(std::string(56, 'a')) ==
          "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK(hashOf(std::string(64, 'a')) ==
          "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}
