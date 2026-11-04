// The Miles digital-sample API the game imports from MSS32.DLL (the AIL_* rows of
// src/hle/surface.tsv), on the Audio component. The game plays every Miles sample double-buffered:
// AIL_sample_buffer_ready names the buffer (0 or 1) to fill next, AIL_load_sample_buffer fills it
// and starts the sample, and a load of zero bytes marks the end of the data. A sample is one Audio
// stream here; each loaded buffer is converted to 16-bit PCM (8-bit, signed or unsigned, and IMA
// ADPCM decoded; swapped stereo put in order) and queued, so the game may reuse its memory at
// once. A starved sample (both buffers played, no end marker) reads as done and plays again when
// the next buffer is loaded.
//
// Scales: volume and master volume 0 (silent) to 127 (as recorded), linear; pan 0 (left) to 127
// (right), 64 the centre. Every method may be called with a handle that is not live; it is then
// ignored, as Miles ignores it. Not thread-safe: the guest shims call it from the game's threads
// under their own lock.
#pragma once

#include "platform/Audio/Audio.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sfcop::hle::miles {

// AIL_sample_status values.
constexpr std::uint32_t kSmpFree = 1;
constexpr std::uint32_t kSmpDone = 2;
constexpr std::uint32_t kSmpPlaying = 4;
// AIL_set_sample_type formats and flags.
constexpr std::int32_t kDigF16Bits = 1;
constexpr std::int32_t kDigFStereo = 2;
constexpr std::int32_t kDigFAdpcm = 4;
constexpr std::uint32_t kDigPcmSign = 1;
constexpr std::uint32_t kDigPcmOrder = 2;

constexpr std::uint32_t kMaxSamples = 64;
constexpr std::uint32_t kUserDataSlots = 8;
// The rate the output is opened at, stereo.
constexpr std::uint32_t kOutputRate = 44100;

class Miles {
public:
    explicit Miles(platform::Audio &audio);
    ~Miles();
    Miles(const Miles &) = delete;
    Miles &operator=(const Miles &) = delete;

    // AIL_startup: 1. AIL_shutdown: every sample released and the output closed.
    std::int32_t Startup();
    void Shutdown();

    // AIL_waveOutOpen: opens the output (once); 0 on success, 1 when no output opens (the
    // reason is then AIL_last_error's). AIL_waveOutClose: every sample stops and reads as done.
    std::int32_t WaveOutOpen();
    void WaveOutClose();

    // AIL_allocate_sample_handle: a handle 1 to kMaxSamples, initialized; 0 when all are taken.
    std::uint32_t AllocateSampleHandle();
    // AIL_release_sample_handle: stops the sample; its handle then reads as free.
    void ReleaseSampleHandle(std::uint32_t sample);
    // AIL_init_sample: stops the sample and restores the defaults: 16-bit mono, 22050 Hz,
    // volume 127, pan 64, no ADPCM block size, user data kept.
    void InitSample(std::uint32_t sample);
    // AIL_end_sample: stops the sample and discards its buffers; it reads as done.
    void EndSample(std::uint32_t sample);

    void SetSampleType(std::uint32_t sample, std::int32_t format, std::uint32_t flags);
    void SetSamplePlaybackRate(std::uint32_t sample, std::int32_t hz);
    void SetSampleAdpcmBlockSize(std::uint32_t sample, std::uint32_t bytes);
    void SetSampleVolume(std::uint32_t sample, std::int32_t volume); // clamped to 0-127
    std::int32_t SampleVolume(std::uint32_t sample);                 // 0 for a dead handle
    void SetSamplePan(std::uint32_t sample, std::int32_t pan);       // clamped to 0-127
    void SetDigitalMasterVolume(std::int32_t volume);                // clamped to 0-127
    // Slots 0 to 7; another index is ignored, and reads 0.
    void SetSampleUserData(std::uint32_t sample, std::uint32_t index, std::int32_t value);
    std::int32_t SampleUserData(std::uint32_t sample, std::uint32_t index);

    // kSmpFree for a dead handle; kSmpPlaying while a loaded buffer is still to be heard;
    // kSmpDone otherwise.
    std::uint32_t SampleStatus(std::uint32_t sample);

    // AIL_minimum_sample_buffer_size: about 50 ms of `format` at `hz` (22050 if not positive),
    // rounded up to 2 KB, at least 2 KB. ADPCM counts a byte a frame.
    std::uint32_t MinimumSampleBufferSize(std::int32_t hz, std::int32_t format) const;

    // AIL_sample_buffer_ready: the buffer to fill next, 0 or 1, or -1 when both are waiting to
    // be played or the end has been marked.
    std::int32_t SampleBufferReady(std::uint32_t sample);

    // AIL_load_sample_buffer: queues `bytes` of the sample's format as buffer `buffer` (0 or 1)
    // and starts the sample if it is not playing; zero bytes (or no data) marks the end.
    void LoadSampleBuffer(std::uint32_t sample, std::uint32_t buffer, const std::uint8_t *data,
                          std::uint32_t bytes);

    // AIL_serve: drains the Audio completion queue (the mixer has its own thread).
    void Serve();

    // AIL_last_error: why the last call failed, "" if none did.
    const std::string &LastError() const { return lastError_; }

    // AIL_set_DirectSound_HWND: 0; the output needs no window.
    std::int32_t SetDirectSoundHwnd(std::uint32_t driver, std::uint32_t window);

    // AIL_set_redist_directory: remembers `directory` (when not null) and returns the previous one.
    std::string SetRedistDirectory(const char *directory);

private:
    struct Sample {
        bool used = false;
        std::int32_t format = kDigF16Bits;
        std::uint32_t flags = 0;
        std::int32_t rate = 22050;
        std::uint32_t adpcmBlock = 0;
        std::int32_t volume = 127;
        std::int32_t pan = 64;
        std::array<std::int32_t, kUserDataSlots> user{};
        platform::StreamId stream;
        platform::ClipFormat streamFormat;
        std::uint32_t nextLoad = 0;
        bool ended = false;
        bool started = false;
    };

    Sample *find(std::uint32_t sample);
    void reset(Sample &s);
    void stop(Sample &s);
    void applyMix(const Sample &s);
    bool ensureStream(Sample &s, const platform::ClipFormat &format);
    std::size_t queued(const Sample &s);

    platform::Audio &audio_;
    bool open_ = false;
    std::int32_t master_ = 127;
    std::array<Sample, kMaxSamples> samples_{};
    std::string lastError_;
    std::string redist_;
};

// A loaded buffer as interleaved little-endian 16-bit PCM, per the sample's format and flags.
std::vector<std::uint8_t> toPcm16(std::int32_t format, std::uint32_t flags,
                                  std::uint32_t adpcmBlock, const std::uint8_t *data,
                                  std::uint32_t bytes);

// Miles' 0-127 pan as the Audio component's -1 to +1.
float panFromMiles(std::int32_t pan);

} // namespace sfcop::hle::miles
