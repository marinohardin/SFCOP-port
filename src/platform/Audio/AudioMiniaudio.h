// The Audio backend on miniaudio (DESIGN §5.8): clips, voices and streams mixed by this backend
// on miniaudio's device thread, with miniaudio's spatializer for the 3D voices
// (AudioSpatial.h). makeAudio() returns one with the default output; this header is for the
// callers that choose the output, the client's --silent switch and the tests.
//
// Mixing: each voice is resampled from its rate to the output rate by linear interpolation, then
// a 2D voice gets its gain and DirectSound's pan law (the far side attenuated by pan * 10000
// hundredths of a decibel, the near side at unity, for mono and stereo alike) and a 3D voice is
// downmixed to mono and spatialized.
// Streams mix as 2D voices. The master gain applies last; the sum is not limited.
#pragma once

#include "platform/Audio/Audio.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace sfcop::platform {

enum class AudioOutput : std::uint8_t {
    kDevice,      // the system's default output; the null device when none opens
    kNullDevice,  // no sound: a device thread consumes the mix in real time, so timing behaves
    kManualClock, // no device and no thread: the mix advances only in Render(), for tests
};

class AudioMiniaudio final : public Audio {
public:
    explicit AudioMiniaudio(AudioOutput output = AudioOutput::kDevice);
    ~AudioMiniaudio() override;
    AudioMiniaudio(const AudioMiniaudio &) = delete;
    AudioMiniaudio &operator=(const AudioMiniaudio &) = delete;

    Status Open(const OutputDesc &desc) override;
    Status Close() override;
    Result<ClipId> LoadClip(const ClipFormat &format, const void *data,
                            std::size_t bytes) override;
    Status WriteClip(ClipId clip, std::size_t offsetBytes, const void *data,
                     std::size_t bytes) override;
    Status ReleaseClip(ClipId clip) override;
    Result<VoiceId> CreateVoice(ClipId clip) override;
    Status DestroyVoice(VoiceId voice) override;
    Status Play(VoiceId voice, bool loop) override;
    Status Stop(VoiceId voice) override;
    Status SetPosition(VoiceId voice, std::uint64_t frame) override;
    Result<VoiceStatus> GetStatus(VoiceId voice) override;
    Status SetGain(VoiceId voice, float gain) override;
    Status SetPan(VoiceId voice, float pan) override;
    Status SetRate(VoiceId voice, std::uint32_t hz) override;
    Status SetVoice3D(VoiceId voice, const Voice3D &params) override;
    Status SetListener(const Listener &listener) override;
    Result<StreamId> OpenStream(const ClipFormat &format) override;
    Status QueueBuffer(StreamId stream, const void *data, std::size_t bytes) override;
    Result<std::size_t> QueuedBuffers(StreamId stream) override;
    Status PlayStream(StreamId stream) override;
    Status StopStream(StreamId stream) override;
    Status FlushStream(StreamId stream) override;
    Status SetStreamGain(StreamId stream, float gain) override;
    Status SetStreamPan(StreamId stream, float pan) override;
    Status CloseStream(StreamId stream) override;
    Status SetMasterGain(float gain) override;
    Result<std::optional<Completion>> PollCompletion() override;

    // kManualClock only: mixes `frames` output frames now, on the caller's thread, writing them
    // interleaved into `out` (the open output's channels per frame) unless `out` is null.
    // Errors: kWrongState if not open or not in manual-clock mode.
    Status Render(float *out, std::size_t frames);

    // Whether the open output is the null device (asked for, or no device opened).
    bool Silent() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sfcop::platform
