#include "platform/Audio/AudioMixer.h"

#include "platform/Audio/ImaAdpcm.h"

#include <algorithm>
#include <cmath>

namespace sfcop::platform::mixer {
namespace {

// The sample at a fractional position, between `frame` and the one after it (`next`, which the
// caller finds: the following frame, the loop's first or the next buffer's first).
float lerp(float a, float b, double fraction) {
    return a + (b - a) * static_cast<float>(fraction);
}

} // namespace

std::size_t Pcm::FrameBytes() const {
    return static_cast<std::size_t>(channels) * (format == SampleFormat::kPcm8 ? 1u : 2u);
}

float Pcm::Sample(std::uint64_t frame, unsigned channel) const {
    if (channel >= channels) {
        channel = 0;
    }
    std::size_t at = static_cast<std::size_t>(frame) * FrameBytes();
    if (format == SampleFormat::kPcm8) {
        return (static_cast<float>(bytes[at + channel]) - 128.0f) / 128.0f;
    }
    at += 2u * channel;
    auto value = static_cast<std::int16_t>(bytes[at] | (bytes[at + 1] << 8));
    return static_cast<float>(value) / 32768.0f;
}

Pcm makePcm(const ClipFormat &format, const void *data, std::size_t bytes) {
    Pcm pcm;
    pcm.channels = format.channels;
    const auto *in = static_cast<const std::uint8_t *>(data);
    if (format.format == SampleFormat::kImaAdpcm) {
        std::vector<std::int16_t> decoded =
            decodeImaAdpcm(in, bytes, format.adpcmBlockBytes, format.channels);
        pcm.format = SampleFormat::kPcm16;
        pcm.bytes.resize(decoded.size() * 2);
        for (std::size_t i = 0; i < decoded.size(); ++i) {
            auto u = static_cast<std::uint16_t>(decoded[i]);
            pcm.bytes[2 * i] = static_cast<std::uint8_t>(u & 0xFFu);
            pcm.bytes[2 * i + 1] = static_cast<std::uint8_t>(u >> 8);
        }
    } else {
        pcm.format = format.format;
        pcm.bytes.assign(in, in + bytes);
    }
    pcm.frames = pcm.bytes.size() / pcm.FrameBytes();
    return pcm;
}

Mixer::Mixer(unsigned outChannels, std::uint32_t outRate)
    : channels(outChannels), sampleRate(outRate), scene(outChannels, outRate) {}

void Mixer::Complete(const Completion &completion) {
    if (completions_.size() < kCompletionLimit) {
        completions_.push_back(completion);
    }
}

std::optional<Completion> Mixer::NextCompletion() {
    if (completions_.empty()) {
        return std::nullopt;
    }
    Completion next = completions_.front();
    completions_.pop_front();
    return next;
}

void Mixer::ForgetVoice(VoiceId voice) {
    completions_.erase(std::remove_if(completions_.begin(), completions_.end(),
                                      [voice](const Completion &c) {
                                          return c.kind == CompletionKind::kVoiceEnded &&
                                                 c.voice == voice;
                                      }),
                       completions_.end());
}

void Mixer::ForgetStream(StreamId stream) {
    completions_.erase(std::remove_if(completions_.begin(), completions_.end(),
                                      [stream](const Completion &c) {
                                          return c.kind == CompletionKind::kStreamBufferDone &&
                                                 c.stream == stream;
                                      }),
                       completions_.end());
}

void Mixer::Mix(float *out, std::size_t frames) {
    sum_.assign(frames * channels, 0.0f);
    for (auto &[id, voice] : voices) {
        if (voice.playing) {
            mixVoice(id, voice, frames);
        }
    }
    for (auto &[id, stream] : streams) {
        if (stream.playing) {
            mixStream(id, stream, frames);
        }
    }
    if (out != nullptr) {
        for (std::size_t i = 0; i < sum_.size(); ++i) {
            out[i] = sum_[i] * masterGain;
        }
    }
}

// Adds one source frame to the mix with a gain and DirectSound's pan law: the pan is the number of
// hundredths of a decibel (pan * 10000) the opposite side is attenuated by while the nearer side
// stays at unity, so a centred source is at unity on both sides. A full pan silences the far side.
void Mixer::addPanned(std::size_t frame, float left, float right, unsigned,
                      float gain, float pan) {
    float *o = &sum_[frame * channels];
    if (channels == 1) {
        o[0] += 0.5f * (left + right) * gain;
        return;
    }
    float gl = 1.0f;
    float gr = 1.0f;
    float far = std::fabs(pan) >= 1.0f ? 0.0f : std::pow(10.0f, -std::fabs(pan) * 5.0f);
    if (pan > 0.0f) {
        gl = far;
    } else if (pan < 0.0f) {
        gr = far;
    }
    o[0] += left * gl * gain;
    o[1] += right * gr * gain;
}

void Mixer::mixVoice(std::uint32_t id, VoiceState &voice, std::size_t frames) {
    auto found = clips.find(voice.clip);
    if (found == clips.end() || found->second.pcm.frames == 0) {
        voice.playing = false;
        return;
    }
    const Pcm &pcm = found->second.pcm;
    const auto end = static_cast<double>(pcm.frames);
    const bool spatial = voice.params.space != VoiceSpace::k2d && voice.source;
    double step = static_cast<double>(voice.rate) / sampleRate;
    if (spatial) {
        step *= scene.DopplerPitch(*voice.source);
        mono_.assign(frames, 0.0f);
    }
    std::size_t produced = 0;
    bool ended = false;
    for (; produced < frames; ++produced) {
        if (voice.position >= end) {
            if (!voice.loop) {
                ended = true;
                break;
            }
            voice.position = std::fmod(voice.position, end);
        }
        auto frame = static_cast<std::uint64_t>(voice.position);
        double fraction = voice.position - static_cast<double>(frame);
        std::uint64_t next = frame + 1 < pcm.frames ? frame + 1 : (voice.loop ? 0 : frame);
        float l = lerp(pcm.Sample(frame, 0), pcm.Sample(next, 0), fraction);
        float r = pcm.channels == 2 ? lerp(pcm.Sample(frame, 1), pcm.Sample(next, 1), fraction) : l;
        if (spatial) {
            mono_[produced] = 0.5f * (l + r);
        } else {
            addPanned(produced, l, r, pcm.channels, voice.gain, voice.pan);
        }
        voice.position += step;
    }
    if (!voice.loop && voice.position >= end) {
        ended = true;
    }
    if (spatial && produced > 0) {
        spat_.assign(produced * channels, 0.0f);
        scene.Process(*voice.source, mono_.data(), spat_.data(), produced);
        for (std::size_t i = 0; i < spat_.size(); ++i) {
            sum_[i] += spat_[i] * voice.gain;
        }
    }
    if (ended) {
        voice.playing = false;
        voice.position = end;
        Completion done;
        done.kind = CompletionKind::kVoiceEnded;
        done.voice = VoiceId{id};
        Complete(done);
    }
}

void Mixer::mixStream(std::uint32_t id, StreamState &stream, std::size_t frames) {
    const double step = static_cast<double>(stream.format.sampleRate) / sampleRate;
    auto retireFinished = [&]() {
        while (!stream.queue.empty() &&
               stream.position >= static_cast<double>(stream.queue.front().frames)) {
            stream.position -= static_cast<double>(stream.queue.front().frames);
            stream.queue.pop_front();
            Completion done;
            done.kind = CompletionKind::kStreamBufferDone;
            done.stream = StreamId{id};
            Complete(done);
        }
        if (stream.queue.empty()) {
            stream.position = 0.0; // starved: silence until the next buffer, still playing
        }
    };
    for (std::size_t f = 0; f < frames; ++f) {
        retireFinished();
        if (stream.queue.empty()) {
            return;
        }
        const Pcm &front = stream.queue.front();
        auto frame = static_cast<std::uint64_t>(stream.position);
        double fraction = stream.position - static_cast<double>(frame);
        const Pcm *nextBuffer = &front;
        std::uint64_t next = frame + 1;
        if (next >= front.frames) {
            nextBuffer = stream.queue.size() > 1 ? &stream.queue[1] : &front;
            next = stream.queue.size() > 1 ? 0 : frame;
        }
        float l = lerp(front.Sample(frame, 0), nextBuffer->Sample(next, 0), fraction);
        float r = front.channels == 2
                      ? lerp(front.Sample(frame, 1), nextBuffer->Sample(next, 1), fraction)
                      : l;
        addPanned(f, l, r, front.channels, stream.gain, stream.pan);
        stream.position += step;
    }
    retireFinished();
}

} // namespace sfcop::platform::mixer
