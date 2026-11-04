#include "platform/Audio/ImaAdpcm.h"

#include <algorithm>

namespace sfcop::platform {
namespace {

// The step table and index adjustments of the IMA ADPCM reference algorithm.
constexpr std::int32_t kStep[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,   253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,   876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
constexpr std::int32_t kIndexStep[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

struct ChannelState {
    std::int32_t predictor = 0;
    std::int32_t index = 0;

    std::int16_t Next(unsigned nibble) {
        std::int32_t step = kStep[index];
        std::int32_t diff = step >> 3;
        if (nibble & 1u) {
            diff += step >> 2;
        }
        if (nibble & 2u) {
            diff += step >> 1;
        }
        if (nibble & 4u) {
            diff += step;
        }
        predictor += (nibble & 8u) ? -diff : diff;
        predictor = std::clamp(predictor, std::int32_t{-32768}, std::int32_t{32767});
        index = std::clamp(index + kIndexStep[nibble & 7u], std::int32_t{0}, std::int32_t{88});
        return static_cast<std::int16_t>(predictor);
    }
};

// Decodes one block of `n` bytes (n may be short) and appends its frames to `out`.
void decodeBlock(const std::uint8_t *block, std::size_t n, unsigned channels,
                 std::vector<std::int16_t> &out) {
    const std::size_t header = 4u * channels;
    if (n < header) {
        return;
    }
    ChannelState state[2];
    for (unsigned c = 0; c < channels; ++c) {
        const std::uint8_t *h = block + 4u * c;
        state[c].predictor = static_cast<std::int16_t>(h[0] | (h[1] << 8));
        state[c].index = std::min<std::int32_t>(h[2], 88);
        out.push_back(static_cast<std::int16_t>(state[c].predictor));
    }
    const std::uint8_t *data = block + header;
    const std::size_t dataBytes = n - header;
    if (channels == 1) {
        for (std::size_t k = 0; k < dataBytes; ++k) {
            out.push_back(state[0].Next(data[k] & 0x0Fu));
            out.push_back(state[0].Next(data[k] >> 4));
        }
        return;
    }
    // Stereo: each 8-byte group holds 8 samples of the left channel, then 8 of the right.
    for (std::size_t g = 0; g + 8 <= dataBytes; g += 8) {
        std::int16_t frames[8][2];
        for (unsigned c = 0; c < 2; ++c) {
            for (unsigned k = 0; k < 4; ++k) {
                std::uint8_t byte = data[g + 4u * c + k];
                frames[2 * k][c] = state[c].Next(byte & 0x0Fu);
                frames[2 * k + 1][c] = state[c].Next(byte >> 4);
            }
        }
        for (const auto &frame : frames) {
            out.push_back(frame[0]);
            out.push_back(frame[1]);
        }
    }
}

} // namespace

std::size_t imaAdpcmFramesPerBlock(std::size_t blockBytes, unsigned channels) {
    if ((channels != 1 && channels != 2) || blockBytes < 4u * channels) {
        return 0;
    }
    const std::size_t dataBytes = blockBytes - 4u * channels;
    if (channels == 1) {
        return 1 + dataBytes * 2;
    }
    return 1 + (dataBytes / 8) * 8;
}

std::vector<std::int16_t> decodeImaAdpcm(const std::uint8_t *data, std::size_t bytes,
                                         std::size_t blockBytes, unsigned channels) {
    std::vector<std::int16_t> out;
    if ((channels != 1 && channels != 2) || blockBytes == 0 || data == nullptr) {
        return out;
    }
    out.reserve(((bytes + blockBytes - 1) / blockBytes) *
                imaAdpcmFramesPerBlock(blockBytes, channels) * channels);
    for (std::size_t offset = 0; offset < bytes; offset += blockBytes) {
        decodeBlock(data + offset, std::min(blockBytes, bytes - offset), channels, out);
    }
    return out;
}

} // namespace sfcop::platform
