#include "hle/miles/Miles.h"

#include "platform/Audio/ImaAdpcm.h"

#include "platform/Core/Log.h"

#include <algorithm>
#include <chrono>

namespace sfcop::hle::miles {
namespace {

using platform::ClipFormat;
using platform::Error;
using platform::SampleFormat;
using platform::StreamId;

void put16(std::vector<std::uint8_t> &out, std::int32_t value) {
    auto u = static_cast<std::uint16_t>(static_cast<std::int16_t>(value));
    out.push_back(static_cast<std::uint8_t>(u & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(u >> 8));
}

// Seconds since the first Miles call, for the detail log.
double logSeconds() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

std::vector<std::uint8_t> toPcm16(std::int32_t format, std::uint32_t flags,
                                  std::uint32_t adpcmBlock, const std::uint8_t *data,
                                  std::uint32_t bytes) {
    const unsigned channels = (format & kDigFStereo) ? 2 : 1;
    std::vector<std::uint8_t> out;
    if (format & kDigFAdpcm) {
        // A block size Miles was not told (or one too small for its headers) is the whole load.
        std::size_t block = adpcmBlock >= 4u * channels ? adpcmBlock : bytes;
        for (std::int16_t s : platform::decodeImaAdpcm(data, bytes, block, channels)) {
            put16(out, s);
        }
    } else if (format & kDigF16Bits) {
        std::uint32_t whole = bytes / (2u * channels) * (2u * channels);
        out.assign(data, data + whole);
    } else {
        std::uint32_t whole = bytes / channels * channels;
        out.reserve(2u * whole);
        for (std::uint32_t i = 0; i < whole; ++i) {
            std::int32_t v = (flags & kDigPcmSign) ? static_cast<std::int8_t>(data[i])
                                                   : static_cast<std::int32_t>(data[i]) - 128;
            put16(out, v * 256);
        }
    }
    if (channels == 2 && (flags & kDigPcmOrder)) {
        for (std::size_t f = 0; f + 4 <= out.size(); f += 4) {
            std::swap(out[f], out[f + 2]);
            std::swap(out[f + 1], out[f + 3]);
        }
    }
    return out;
}

float panFromMiles(std::int32_t pan) {
    pan = std::clamp(pan, 0, 127);
    if (pan <= 64) {
        return static_cast<float>(pan - 64) / 64.0f;
    }
    return static_cast<float>(pan - 64) / 63.0f;
}

Miles::Miles(platform::Audio &audio) : audio_(audio) {}

Miles::~Miles() { Shutdown(); }

std::int32_t Miles::Startup() { return 1; }

void Miles::Shutdown() {
    for (std::uint32_t i = 1; i <= kMaxSamples; ++i) {
        ReleaseSampleHandle(i);
    }
    if (open_) {
        (void)audio_.Close();
        open_ = false;
    }
}

std::int32_t Miles::WaveOutOpen() {
    if (open_) {
        return 0;
    }
    platform::OutputDesc desc;
    desc.sampleRate = kOutputRate;
    desc.channels = 2;
    platform::Status opened = audio_.Open(desc);
    if (!opened.Ok() && opened.Code() != Error::kWrongState) {
        lastError_ = "no digital output device";
        return 1;
    }
    open_ = true;
    (void)audio_.SetMasterGain(static_cast<float>(master_) / 127.0f);
    return 0;
}

void Miles::WaveOutClose() {
    for (Sample &s : samples_) {
        if (s.used) {
            stop(s);
        }
    }
}

std::uint32_t Miles::AllocateSampleHandle() {
    sfcop_logf(SFCOP_LOG_DETAIL, "[mss] %.3f allocate sample handle\n", logSeconds());
    for (std::uint32_t i = 0; i < kMaxSamples; ++i) {
        Sample &s = samples_[i];
        if (!s.used) {
            s = Sample{};
            s.used = true;
            return i + 1;
        }
    }
    lastError_ = "out of sample handles";
    return 0;
}

Miles::Sample *Miles::find(std::uint32_t sample) {
    if (sample == 0 || sample > kMaxSamples || !samples_[sample - 1].used) {
        return nullptr;
    }
    return &samples_[sample - 1];
}

// Stops playback and drops what is queued; the stream stays open for the next load.
void Miles::stop(Sample &s) {
    sfcop_logf(SFCOP_LOG_DETAIL, "[mss] %.3f stop sample %ld: started %d, ended %d, %u loads\n",
               logSeconds(), static_cast<long>(&s - samples_.data()) + 1, s.started, s.ended,
               s.loads);
    s.seenReady = -2;
    s.seenStatus = 0;
    s.loads = 0;
    if (s.stream.Valid()) {
        (void)audio_.StopStream(s.stream);
        (void)audio_.FlushStream(s.stream);
    }
    s.started = false;
    s.ended = false;
    s.nextLoad = 0;
}

void Miles::reset(Sample &s) {
    stop(s);
    s.format = kDigF16Bits;
    s.flags = 0;
    s.rate = 22050;
    s.adpcmBlock = 0;
    s.volume = 127;
    s.pan = 64;
}

void Miles::ReleaseSampleHandle(std::uint32_t sample) {
    Sample *s = find(sample);
    if (s == nullptr) {
        return;
    }
    if (s->stream.Valid()) {
        (void)audio_.CloseStream(s->stream);
    }
    *s = Sample{};
}

void Miles::InitSample(std::uint32_t sample) {
    if (Sample *s = find(sample)) {
        reset(*s);
    }
}

void Miles::EndSample(std::uint32_t sample) {
    if (Sample *s = find(sample)) {
        stop(*s);
    }
}

void Miles::SetSampleType(std::uint32_t sample, std::int32_t format, std::uint32_t flags) {
    if (Sample *s = find(sample)) {
        s->format = format;
        s->flags = flags;
    }
}

void Miles::SetSamplePlaybackRate(std::uint32_t sample, std::int32_t hz) {
    if (Sample *s = find(sample)) {
        s->rate = hz;
    }
}

void Miles::SetSampleAdpcmBlockSize(std::uint32_t sample, std::uint32_t bytes) {
    if (Sample *s = find(sample)) {
        s->adpcmBlock = bytes;
    }
}

void Miles::applyMix(const Sample &s) {
    if (s.stream.Valid()) {
        (void)audio_.SetStreamGain(s.stream, static_cast<float>(s.volume) / 127.0f);
        (void)audio_.SetStreamPan(s.stream, panFromMiles(s.pan));
    }
}

void Miles::SetSampleVolume(std::uint32_t sample, std::int32_t volume) {
    if (Sample *s = find(sample)) {
        s->volume = std::clamp(volume, 0, 127);
        applyMix(*s);
    }
}

std::int32_t Miles::SampleVolume(std::uint32_t sample) {
    Sample *s = find(sample);
    return s ? s->volume : 0;
}

void Miles::SetSamplePan(std::uint32_t sample, std::int32_t pan) {
    if (Sample *s = find(sample)) {
        s->pan = std::clamp(pan, 0, 127);
        applyMix(*s);
    }
}

void Miles::SetDigitalMasterVolume(std::int32_t volume) {
    master_ = std::clamp(volume, 0, 127);
    if (open_) {
        (void)audio_.SetMasterGain(static_cast<float>(master_) / 127.0f);
    }
}

void Miles::SetSampleUserData(std::uint32_t sample, std::uint32_t index, std::int32_t value) {
    Sample *s = find(sample);
    if (s != nullptr && index < kUserDataSlots) {
        s->user[index] = value;
    }
}

std::int32_t Miles::SampleUserData(std::uint32_t sample, std::uint32_t index) {
    Sample *s = find(sample);
    return s != nullptr && index < kUserDataSlots ? s->user[index] : 0;
}

std::size_t Miles::queued(const Sample &s) {
    if (!s.stream.Valid()) {
        return 0;
    }
    return audio_.QueuedBuffers(s.stream).ValueOr(0);
}

std::uint32_t Miles::SampleStatus(std::uint32_t sample) {
    Serve();
    Sample *s = find(sample);
    if (s == nullptr) {
        return kSmpFree;
    }
    std::uint32_t status = s->started && queued(*s) > 0 ? kSmpPlaying : kSmpDone;
    if (status != s->seenStatus) {
        sfcop_logf(SFCOP_LOG_DETAIL, "[mss] %.3f sample %u status %u -> %u (started %d, ended %d, queued %zu)\n",
                   logSeconds(), sample, s->seenStatus, status, s->started, s->ended, queued(*s));
        s->seenStatus = status;
    }
    return status;
}

std::uint32_t Miles::MinimumSampleBufferSize(std::int32_t hz, std::int32_t format) const {
    std::uint32_t rate = hz > 0 ? static_cast<std::uint32_t>(hz) : 22050u;
    std::uint32_t frameBytes =
        ((format & kDigF16Bits) ? 2u : 1u) * ((format & kDigFStereo) ? 2u : 1u);
    if (format & kDigFAdpcm) {
        frameBytes = 1;
    }
    std::uint32_t bytes = (rate * frameBytes / 20 + 2047u) & ~2047u;
    return (std::max)(bytes, 2048u);
}

std::int32_t Miles::SampleBufferReady(std::uint32_t sample) {
    Serve();
    Sample *s = find(sample);
    if (s == nullptr) {
        return -1;
    }
    std::int32_t ready = s->ended || (s->started && queued(*s) >= 2)
                             ? -1
                             : static_cast<std::int32_t>(s->nextLoad);
    if (ready != s->seenReady) {
        sfcop_logf(SFCOP_LOG_DETAIL, "[mss] %.3f sample %u buffer ready %d -> %d (started %d, ended %d, queued %zu)\n",
                   logSeconds(), sample, s->seenReady, ready, s->started, s->ended, queued(*s));
        s->seenReady = ready;
    }
    return ready;
}

bool Miles::ensureStream(Sample &s, const ClipFormat &format) {
    if (s.stream.Valid() && s.streamFormat.channels == format.channels &&
        s.streamFormat.sampleRate == format.sampleRate) {
        return true;
    }
    if (s.stream.Valid()) {
        (void)audio_.CloseStream(s.stream);
        s.stream = StreamId{};
    }
    platform::Result<StreamId> opened = audio_.OpenStream(format);
    if (!opened.Ok()) {
        lastError_ = "the sample cannot be played";
        return false;
    }
    s.stream = opened.Value();
    s.streamFormat = format;
    s.started = false; // a new stream is played from its first buffer
    applyMix(s);
    return true;
}

void Miles::LoadSampleBuffer(std::uint32_t sample, std::uint32_t buffer,
                             const std::uint8_t *data, std::uint32_t bytes) {
    Sample *s = find(sample);
    if (s == nullptr || buffer > 1) {
        return;
    }
    sfcop_logf(SFCOP_LOG_DETAIL, "[mss] %.3f load sample %u buffer %u: %u bytes, format %d, %d Hz, load %u\n",
               logSeconds(), sample, buffer, bytes, s->format, s->rate, s->loads);
    ++s->loads;
    if (data == nullptr || bytes == 0) {
        s->ended = true; // no more data: the sample is done once the rest is heard
        return;
    }
    s->ended = false;
    s->nextLoad = buffer ^ 1u;
    std::vector<std::uint8_t> pcm = toPcm16(s->format, s->flags, s->adpcmBlock, data, bytes);
    ClipFormat format;
    format.format = SampleFormat::kPcm16;
    format.channels = (s->format & kDigFStereo) ? 2 : 1;
    format.sampleRate =
        static_cast<std::uint32_t>(std::clamp(s->rate > 0 ? s->rate : 22050, 1000, 192000));
    if (pcm.empty() || !ensureStream(*s, format)) {
        return;
    }
    if (!audio_.QueueBuffer(s->stream, pcm.data(), pcm.size()).Ok()) {
        sfcop_logf(SFCOP_LOG_DETAIL, "[mss] %.3f sample %u: the buffer was not queued\n", logSeconds(),
                   sample);
        return;
    }
    if (!s->started) {
        s->started = audio_.PlayStream(s->stream).Ok();
    }
}

void Miles::Serve() {
    for (;;) {
        platform::Result<std::optional<platform::Completion>> next = audio_.PollCompletion();
        if (!next.Ok() || !next.Value()) {
            return;
        }
    }
}

std::int32_t Miles::SetDirectSoundHwnd(std::uint32_t, std::uint32_t) { return 0; }

std::string Miles::SetRedistDirectory(const char *directory) {
    std::string previous = redist_;
    if (directory != nullptr) {
        redist_ = directory;
    }
    return previous;
}

} // namespace sfcop::hle::miles
