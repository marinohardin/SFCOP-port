// IMA ADPCM in the WAVE block layout, decoded to signed 16-bit PCM. Each block starts with one
// 4-byte header per channel (the first sample, little-endian, then the step index and a reserved
// byte); the header's sample is the block's first frame. Mono data follows as bytes of two
// samples, low nibble first; stereo data as alternating 4-byte groups of eight samples per
// channel, left first. Used by the Audio backend for kImaAdpcm clips and stream buffers, and by
// the Miles module for the sample buffers the game loads.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sfcop::platform {

// Frames a whole block of `blockBytes` holds for `channels` (1 or 2); 0 if the block is too
// short for its headers.
std::size_t imaAdpcmFramesPerBlock(std::size_t blockBytes, unsigned channels);

// Decodes `bytes` of blocks of `blockBytes` into interleaved frames. A last block shorter than
// `blockBytes` yields the frames it holds; a block shorter than its headers yields none. Returns
// an empty vector for channels other than 1 or 2 or a zero block size.
std::vector<std::int16_t> decodeImaAdpcm(const std::uint8_t *data, std::size_t bytes,
                                         std::size_t blockBytes, unsigned channels);

} // namespace sfcop::platform
