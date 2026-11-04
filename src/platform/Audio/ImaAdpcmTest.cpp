#include "platform/Audio/ImaAdpcm.h"

#include <doctest/doctest.h>

#include <vector>

namespace sfcop::platform {

TEST_CASE("an IMA ADPCM mono block decodes its header sample then two samples a byte") {
    // Header: first sample 0, step index 0. Data 0x07: nibble 7 at step 7 adds 0 + 1 + 3 + 7 =
    // 11 and moves the index to 8; nibble 0 at step 16 adds 16 >> 3 = 2.
    const std::uint8_t block[5] = {0x00, 0x00, 0x00, 0x00, 0x07};
    std::vector<std::int16_t> pcm = decodeImaAdpcm(block, sizeof block, sizeof block, 1);
    CHECK(pcm == std::vector<std::int16_t>{0, 11, 13});
    CHECK(imaAdpcmFramesPerBlock(5, 1) == 3);
}

TEST_CASE("a negative IMA nibble subtracts and the predictor saturates at 16 bits") {
    // Header: first sample -32760 (0x8008), index 88 (step 32767). Nibble 0xF subtracts
    // 4095 + 8191 + 16383 + 32767; the predictor stops at -32768. Nibble 0x0 at the same index
    // adds 4095: -28673.
    const std::uint8_t block[5] = {0x08, 0x80, 88, 0x00, 0x0F};
    std::vector<std::int16_t> pcm = decodeImaAdpcm(block, sizeof block, sizeof block, 1);
    CHECK(pcm == std::vector<std::int16_t>{-32760, -32768, -28673});
}

TEST_CASE("an IMA ADPCM stereo block interleaves eight samples a channel per group") {
    // Left header 100 / index 0, right header -100 / index 0, then one group: four bytes of
    // left nibbles (all zero: +0 each at step 7, the index falling to 0), four of right.
    std::vector<std::uint8_t> block = {100, 0, 0, 0, 0x9C, 0xFF, 0, 0};
    block.insert(block.end(), 8, 0x00);
    std::vector<std::int16_t> pcm = decodeImaAdpcm(block.data(), block.size(), block.size(), 2);
    REQUIRE(pcm.size() == 18);
    CHECK(imaAdpcmFramesPerBlock(16, 2) == 9);
    CHECK(pcm[0] == 100);
    CHECK(pcm[1] == -100);
    CHECK(pcm[16] == 100);
    CHECK(pcm[17] == -100);
}

TEST_CASE("a short last IMA block yields the frames it holds") {
    // Two 6-byte blocks expected; the second has its header and one byte only.
    const std::uint8_t data[11] = {0, 0, 0, 0, 0x00, 0x00, 5, 0, 0, 0, 0x00};
    std::vector<std::int16_t> pcm = decodeImaAdpcm(data, sizeof data, 6, 1);
    CHECK(pcm.size() == 5 + 3);
    CHECK(pcm[5] == 5);
}

TEST_CASE("IMA decoding rejects a channel count it does not know") {
    const std::uint8_t block[8] = {};
    CHECK(decodeImaAdpcm(block, sizeof block, sizeof block, 3).empty());
    CHECK(imaAdpcmFramesPerBlock(3, 1) == 0);
}

} // namespace sfcop::platform
