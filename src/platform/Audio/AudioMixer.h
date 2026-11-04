// The miniaudio backend's mixer state and mixing (AudioMiniaudio.h describes the mix). Internal to
// the backend, which validates every argument and holds its lock around every call here.
#pragma once

#include "platform/Audio/Audio.h"
#include "platform/Audio/AudioSpatial.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace sfcop::platform::mixer {

// Sound data in kPcm8 or kPcm16 (ADPCM is decoded on the way in).
struct Pcm {
    SampleFormat format = SampleFormat::kPcm16;
    std::uint8_t channels = 1;
    std::vector<std::uint8_t> bytes;
    std::uint64_t frames = 0;

    std::size_t FrameBytes() const;
    // One sample, -1 to +1.
    float Sample(std::uint64_t frame, unsigned channel) const;
};

// `bytes` of `format` as PCM; the caller has checked the format and the size.
Pcm makePcm(const ClipFormat &format, const void *data, std::size_t bytes);

struct ClipState {
    Pcm pcm;
    std::uint32_t rate = 22050;
    bool adpcm = false;
};

struct VoiceState {
    std::uint32_t clip = 0;
    double position = 0.0; // frames into the clip
    bool playing = false;
    bool loop = false;
    float gain = 1.0f;
    float pan = 0.0f;
    std::uint32_t rate = 22050;
    Voice3D params;
    std::unique_ptr<SpatialScene::Source> source; // while params.space is not k2d
};

struct StreamState {
    ClipFormat format;
    std::deque<Pcm> queue;
    double position = 0.0; // frames into the front buffer
    bool playing = false;
    float gain = 1.0f;
    float pan = 0.0f;
};

class Mixer {
public:
    static constexpr std::size_t kCompletionLimit = 1024;

    Mixer(unsigned channels, std::uint32_t sampleRate);

    // Mixes `frames` output frames into `out` (interleaved, overwritten), or advances time only
    // when `out` is null.
    void Mix(float *out, std::size_t frames);

    void Complete(const Completion &completion);
    std::optional<Completion> NextCompletion();
    void ForgetVoice(VoiceId voice);
    void ForgetStream(StreamId stream);

    unsigned channels;
    std::uint32_t sampleRate;
    float masterGain = 1.0f;
    SpatialScene scene;
    std::map<std::uint32_t, ClipState> clips;
    std::map<std::uint32_t, VoiceState> voices;
    std::map<std::uint32_t, StreamState> streams;

private:
    void mixVoice(std::uint32_t id, VoiceState &voice, std::size_t frames);
    void mixStream(std::uint32_t id, StreamState &stream, std::size_t frames);
    void addPanned(std::size_t frame, float left, float right, unsigned sourceChannels,
                   float gain, float pan);

    std::deque<Completion> completions_;
    std::vector<float> sum_;   // the mix, interleaved
    std::vector<float> mono_;  // a 3D voice before spatializing
    std::vector<float> spat_;  // a 3D voice after
};

} // namespace sfcop::platform::mixer
