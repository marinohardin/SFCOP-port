#include "platform/Audio/AudioMiniaudio.h"

#include "platform/Audio/AudioMixer.h"
#include "platform/Audio/ImaAdpcm.h"

#include <miniaudio.h>

#include <cmath>
#include <mutex>

namespace sfcop::platform {
namespace {

constexpr std::size_t kVoiceLimit = 256;
constexpr std::size_t kStreamLimit = 64; // one for each of the 64 Miles samples
constexpr std::size_t kQueueLimit = 16;
constexpr std::uint32_t kPeriodFrames = 512; // about 12 ms at 44.1 kHz

bool inUnit(float x) { return x >= 0.0f && x <= 1.0f; }  // false for NaN
bool inPan(float x) { return x >= -1.0f && x <= 1.0f; }  // false for NaN
bool zero(const Vector3 &v) { return v.x == 0.0f && v.y == 0.0f && v.z == 0.0f; }
bool finite(const Vector3 &v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool validFormat(const ClipFormat &f) {
    if (f.channels != 1 && f.channels != 2) {
        return false;
    }
    if (f.sampleRate < 1000 || f.sampleRate > 192000) {
        return false;
    }
    switch (f.format) {
    case SampleFormat::kPcm8:
    case SampleFormat::kPcm16:
        return true;
    case SampleFormat::kImaAdpcm:
        return imaAdpcmFramesPerBlock(f.adpcmBlockBytes, f.channels) > 0;
    }
    return false;
}

// Whole frames, or whole blocks for ADPCM, and not empty.
bool wholeUnits(const ClipFormat &f, const void *data, std::size_t bytes) {
    if (data == nullptr || bytes == 0) {
        return false;
    }
    switch (f.format) {
    case SampleFormat::kPcm8:
        return bytes % f.channels == 0;
    case SampleFormat::kPcm16:
        return bytes % (2u * f.channels) == 0;
    case SampleFormat::kImaAdpcm:
        return bytes % f.adpcmBlockBytes == 0;
    }
    return false;
}

} // namespace

struct AudioMiniaudio::Impl {
    explicit Impl(AudioOutput o) : output(o) {}

    AudioOutput output;
    std::mutex lock;
    bool open = false;
    bool silent = false;
    std::unique_ptr<mixer::Mixer> mix;
    Listener listener;
    float masterGain = 1.0f;
    std::uint32_t nextId = 1;

    ma_context context{};
    bool contextOk = false;
    ma_device device{};
    bool deviceOk = false;

    std::uint32_t NewId() {
        std::uint32_t id = nextId++;
        if (nextId == 0) {
            nextId = 1;
        }
        return id;
    }

    static void DataCallback(ma_device *device, void *out, const void *, ma_uint32 frames) {
        auto *self = static_cast<Impl *>(device->pUserData);
        std::lock_guard<std::mutex> guard(self->lock);
        if (self->mix) {
            self->mix->Mix(static_cast<float *>(out), frames);
        }
    }

    // Opens a device on `backends` (all of them when null); false if none opens.
    bool StartDevice(const ma_backend *backends, ma_uint32 count, const OutputDesc &desc) {
        if (ma_context_init(backends, count, nullptr, &context) != MA_SUCCESS) {
            return false;
        }
        contextOk = true;
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = desc.channels;
        config.sampleRate = desc.sampleRate;
        config.periodSizeInFrames = kPeriodFrames;
        config.dataCallback = &Impl::DataCallback;
        config.pUserData = this;
        if (ma_device_init(&context, &config, &device) != MA_SUCCESS) {
            StopDevice();
            return false;
        }
        deviceOk = true;
        if (ma_device_start(&device) != MA_SUCCESS) {
            StopDevice();
            return false;
        }
        return true;
    }

    void StopDevice() {
        if (deviceOk) {
            ma_device_uninit(&device);
            deviceOk = false;
        }
        if (contextOk) {
            ma_context_uninit(&context);
            contextOk = false;
        }
    }

    mixer::VoiceState *Voice(VoiceId id) {
        if (!mix) {
            return nullptr;
        }
        auto found = mix->voices.find(id.value);
        return found == mix->voices.end() ? nullptr : &found->second;
    }
    mixer::StreamState *Stream(StreamId id) {
        if (!mix) {
            return nullptr;
        }
        auto found = mix->streams.find(id.value);
        return found == mix->streams.end() ? nullptr : &found->second;
    }
};

AudioMiniaudio::AudioMiniaudio(AudioOutput output) : impl_(std::make_unique<Impl>(output)) {}

AudioMiniaudio::~AudioMiniaudio() { (void)Close(); }

Status AudioMiniaudio::Open(const OutputDesc &desc) {
    {
        std::lock_guard<std::mutex> guard(impl_->lock);
        if (impl_->open) {
            return Error::kWrongState;
        }
        if (desc.sampleRate < 8000 || desc.sampleRate > 192000 ||
            (desc.channels != 1 && desc.channels != 2)) {
            return Error::kInvalidArgument;
        }
        impl_->mix = std::make_unique<mixer::Mixer>(desc.channels, desc.sampleRate);
        impl_->mix->scene.SetListener(impl_->listener);
        impl_->mix->masterGain = impl_->masterGain;
        impl_->open = true;
        impl_->silent = impl_->output != AudioOutput::kDevice;
    }
    // The device thread takes the lock in its callback, so it is started without the lock held.
    if (impl_->output == AudioOutput::kManualClock) {
        return {};
    }
    bool started = false;
    if (impl_->output == AudioOutput::kDevice) {
        started = impl_->StartDevice(nullptr, 0, desc);
    }
    if (!started) {
        const ma_backend null[] = {ma_backend_null};
        started = impl_->StartDevice(null, 1, desc);
        if (started) {
            std::lock_guard<std::mutex> guard(impl_->lock);
            impl_->silent = true;
        }
    }
    if (!started) {
        std::lock_guard<std::mutex> guard(impl_->lock);
        impl_->mix.reset();
        impl_->open = false;
        return Error::kIoError;
    }
    return {};
}

Status AudioMiniaudio::Close() {
    impl_->StopDevice();
    std::lock_guard<std::mutex> guard(impl_->lock);
    impl_->mix.reset();
    impl_->open = false;
    impl_->silent = false;
    return {};
}

Result<ClipId> AudioMiniaudio::LoadClip(const ClipFormat &format, const void *data,
                                        std::size_t bytes) {
    if (!validFormat(format) || !wholeUnits(format, data, bytes)) {
        std::lock_guard<std::mutex> guard(impl_->lock);
        return impl_->open ? Error::kInvalidArgument : Error::kWrongState;
    }
    // The bytes are copied before the lock is taken: a large clip must not stall the device.
    mixer::ClipState clip;
    clip.pcm = mixer::makePcm(format, data, bytes);
    clip.rate = format.sampleRate;
    clip.adpcm = format.format == SampleFormat::kImaAdpcm;
    std::lock_guard<std::mutex> guard(impl_->lock);
    if (!impl_->open) {
        return Error::kWrongState;
    }
    std::uint32_t id = impl_->NewId();
    impl_->mix->clips.emplace(id, std::move(clip));
    return ClipId{id};
}

Status AudioMiniaudio::WriteClip(ClipId clip, std::size_t offsetBytes, const void *data,
                                 std::size_t bytes) {
    std::lock_guard<std::mutex> guard(impl_->lock);
    if (!impl_->mix) {
        return Error::kInvalidHandle;
    }
    auto found = impl_->mix->clips.find(clip.value);
    if (found == impl_->mix->clips.end()) {
        return Error::kInvalidHandle;
    }
    std::vector<std::uint8_t> &target = found->second.pcm.bytes;
    if (found->second.adpcm || data == nullptr || offsetBytes > target.size() ||
        bytes > target.size() - offsetBytes) {
        return Error::kInvalidArgument;
    }
    const auto *in = static_cast<const std::uint8_t *>(data);
    std::copy(in, in + bytes, target.begin() + static_cast<std::ptrdiff_t>(offsetBytes));
    return {};
}

Status AudioMiniaudio::ReleaseClip(ClipId clip) {
    std::lock_guard<std::mutex> guard(impl_->lock);
    if (!impl_->mix || impl_->mix->clips.erase(clip.value) == 0) {
        return Error::kInvalidHandle;
    }
    auto &voices = impl_->mix->voices;
    for (auto it = voices.begin(); it != voices.end();) {
        if (it->second.clip == clip.value) {
            impl_->mix->ForgetVoice(VoiceId{it->first});
            it = voices.erase(it);
        } else {
            ++it;
        }
    }
    return {};
}

Result<VoiceId> AudioMiniaudio::CreateVoice(ClipId clip) {
    std::lock_guard<std::mutex> guard(impl_->lock);
    if (!impl_->mix) {
        return Error::kInvalidHandle;
    }
    auto found = impl_->mix->clips.find(clip.value);
    if (found == impl_->mix->clips.end()) {
        return Error::kInvalidHandle;
    }
    if (impl_->mix->voices.size() >= kVoiceLimit) {
        return Error::kOutOfResources;
    }
    mixer::VoiceState voice;
    voice.clip = clip.value;
    voice.rate = found->second.rate;
    std::uint32_t id = impl_->NewId();
    impl_->mix->voices.emplace(id, std::move(voice));
    return VoiceId{id};
}

Status AudioMiniaudio::DestroyVoice(VoiceId voice) {
    std::lock_guard<std::mutex> guard(impl_->lock);
    if (!impl_->mix || impl_->mix->voices.erase(voice.value) == 0) {
        return Error::kInvalidHandle;
    }
    impl_->mix->ForgetVoice(voice);
    return {};
}

Status AudioMiniaudio::Play(VoiceId voice, bool loop) {
    std::lock_guard<std::mutex> guard(impl_->lock);
    mixer::VoiceState *v = impl_->Voice(voice);
    if (v == nullptr) {
        return Error::kInvalidHandle;
    }
    v->playing = true;
    v->loop = loop;
    return {};
}

Status AudioMiniaudio::Stop(VoiceId voice) {
    std::lock_guard<std::mutex> guard(impl_->lock);
    mixer::VoiceState *v = impl_->Voice(voice);
    if (v == nullptr) {
        return Error::kInvalidHandle;
    }
    v->playing = false;
    return {};
}

Status AudioMiniaudio::SetPosition(VoiceId voice, std::uint64_t frame) {
    std::lock_guard<std::mutex> guard(impl_->lock);
    mixer::VoiceState *v = impl_->Voice(voice);
    if (v == nullptr) {
        return Error::kInvalidHandle;
    }
    if (frame > impl_->mix->clips.at(v->clip).pcm.frames) {
        return Error::kInvalidArgument;
    }
    v->position = static_cast<double>(frame);
    return {};
}

Result<VoiceStatus> AudioMiniaudio::GetStatus(VoiceId voice) {
    std::lock_guard<std::mutex> guard(impl_->lock);
    mixer::VoiceState *v = impl_->Voice(voice);
    if (v == nullptr) {
        return Error::kInvalidHandle;
    }
    VoiceStatus status;
    status.state = v->playing ? VoiceState::kPlaying : VoiceState::kStopped;
    std::uint64_t frames = impl_->mix->clips.at(v->clip).pcm.frames;
    status.frame = (std::min)(static_cast<std::uint64_t>(v->position), frames);
    return status;
}

Status AudioMiniaudio::SetGain(VoiceId voice, float gain) {
    std::lock_guard<std::mutex> guard(impl_->lock);
    mixer::VoiceState *v = impl_->Voice(voice);
    if (v == nullptr) {
        return Error::kInvalidHandle;
    }
    if (!inUnit(gain)) {
        return Error::kInvalidArgument;
    }
    v->gain = gain;
    return {};
}

Status AudioMiniaudio::SetPan(VoiceId voice, float pan) {
    std::lock_guard<std::mutex> guard(impl_->lock);
    mixer::VoiceState *v = impl_->Voice(voice);
    if (v == nullptr) {
        return Error::kInvalidHandle;
    }
    if (!inPan(pan)) {
        return Error::kInvalidArgument;
    }
    v->pan = pan;
    return {};
}

Status AudioMiniaudio::SetRate(VoiceId voice, std::uint32_t hz) {
    std::lock_guard<std::mutex> guard(impl_->lock);
    mixer::VoiceState *v = impl_->Voice(voice);
    if (v == nullptr) {
        return Error::kInvalidHandle;
    }
    if (hz < 100 || hz > 192000) {
        return Error::kInvalidArgument;
    }
    v->rate = hz;
    return {};
}

Status AudioMiniaudio::SetVoice3D(VoiceId voice, const Voice3D &p) {
    std::lock_guard<std::mutex> guard(impl_->lock);
    mixer::VoiceState *v = impl_->Voice(voice);
    if (v == nullptr) {
        return Error::kInvalidHandle;
    }
    bool distances = p.minDistance >= 0.0f && p.maxDistance >= p.minDistance;
    bool angles = p.coneInsideDegrees <= p.coneOutsideDegrees && p.coneOutsideDegrees <= 360;
    if (!distances || !angles || !inUnit(p.coneOutsideGain) || !finite(p.position) ||
        !finite(p.velocity) || !finite(p.coneOrientation)) {
        return Error::kInvalidArgument;
    }
    v->params = p;
    if (p.space == VoiceSpace::k2d) {
        v->source.reset();
        return {};
    }
    if (!v->source) {
        v->source = impl_->mix->scene.NewSource();
    }
    impl_->mix->scene.SetSource(*v->source, p);
    return {};
}

Status AudioMiniaudio::SetListener(const Listener &l) {
    bool factors = l.distanceFactor > 0.0f && std::isfinite(l.distanceFactor) &&
                   l.rolloffFactor >= 0.0f && l.rolloffFactor <= 10.0f &&
                   l.dopplerFactor >= 0.0f && l.dopplerFactor <= 10.0f;
    if (zero(l.forward) || zero(l.up) || !factors || !finite(l.position) ||
        !finite(l.velocity) || !finite(l.forward) || !finite(l.up)) {
        return Error::kInvalidArgument;
    }
    std::lock_guard<std::mutex> guard(impl_->lock);
    impl_->listener = l;
    if (impl_->mix) {
        impl_->mix->scene.SetListener(l);
    }
    return {};
}

Result<StreamId> AudioMiniaudio::OpenStream(const ClipFormat &format) {
    std::lock_guard<std::mutex> guard(impl_->lock);
    if (!impl_->open) {
        return Error::kWrongState;
    }
    if (!validFormat(format)) {
        return Error::kInvalidArgument;
    }
    if (impl_->mix->streams.size() >= kStreamLimit) {
        return Error::kOutOfResources;
    }
    mixer::StreamState stream;
    stream.format = format;
    std::uint32_t id = impl_->NewId();
    impl_->mix->streams.emplace(id, std::move(stream));
    return StreamId{id};
}

Status AudioMiniaudio::QueueBuffer(StreamId stream, const void *data, std::size_t bytes) {
    ClipFormat format;
    {
        std::lock_guard<std::mutex> guard(impl_->lock);
        mixer::StreamState *s = impl_->Stream(stream);
        if (s == nullptr) {
            return Error::kInvalidHandle;
        }
        format = s->format;
    }
    if (!wholeUnits(format, data, bytes)) {
        return Error::kInvalidArgument;
    }
    mixer::Pcm pcm = mixer::makePcm(format, data, bytes); // outside the lock
    std::lock_guard<std::mutex> guard(impl_->lock);
    mixer::StreamState *s = impl_->Stream(stream); // it may have been closed meanwhile
    if (s == nullptr) {
        return Error::kInvalidHandle;
    }
    if (s->queue.size() >= kQueueLimit) {
        return Error::kOutOfResources;
    }
    s->queue.push_back(std::move(pcm));
    return {};
}

Result<std::size_t> AudioMiniaudio::QueuedBuffers(StreamId stream) {
    std::lock_guard<std::mutex> guard(impl_->lock);
    mixer::StreamState *s = impl_->Stream(stream);
    if (s == nullptr) {
        return Error::kInvalidHandle;
    }
    return s->queue.size();
}

Status AudioMiniaudio::PlayStream(StreamId stream) {
    std::lock_guard<std::mutex> guard(impl_->lock);
    mixer::StreamState *s = impl_->Stream(stream);
    if (s == nullptr) {
        return Error::kInvalidHandle;
    }
    s->playing = true;
    return {};
}

Status AudioMiniaudio::StopStream(StreamId stream) {
    std::lock_guard<std::mutex> guard(impl_->lock);
    mixer::StreamState *s = impl_->Stream(stream);
    if (s == nullptr) {
        return Error::kInvalidHandle;
    }
    s->playing = false;
    return {};
}

Status AudioMiniaudio::FlushStream(StreamId stream) {
    std::lock_guard<std::mutex> guard(impl_->lock);
    mixer::StreamState *s = impl_->Stream(stream);
    if (s == nullptr) {
        return Error::kInvalidHandle;
    }
    s->queue.clear();
    s->position = 0.0;
    return {};
}

Status AudioMiniaudio::SetStreamGain(StreamId stream, float gain) {
    std::lock_guard<std::mutex> guard(impl_->lock);
    mixer::StreamState *s = impl_->Stream(stream);
    if (s == nullptr) {
        return Error::kInvalidHandle;
    }
    if (!inUnit(gain)) {
        return Error::kInvalidArgument;
    }
    s->gain = gain;
    return {};
}

Status AudioMiniaudio::SetStreamPan(StreamId stream, float pan) {
    std::lock_guard<std::mutex> guard(impl_->lock);
    mixer::StreamState *s = impl_->Stream(stream);
    if (s == nullptr) {
        return Error::kInvalidHandle;
    }
    if (!inPan(pan)) {
        return Error::kInvalidArgument;
    }
    s->pan = pan;
    return {};
}

Status AudioMiniaudio::CloseStream(StreamId stream) {
    std::lock_guard<std::mutex> guard(impl_->lock);
    if (!impl_->mix || impl_->mix->streams.erase(stream.value) == 0) {
        return Error::kInvalidHandle;
    }
    impl_->mix->ForgetStream(stream);
    return {};
}

Status AudioMiniaudio::SetMasterGain(float gain) {
    if (!inUnit(gain)) {
        return Error::kInvalidArgument;
    }
    std::lock_guard<std::mutex> guard(impl_->lock);
    impl_->masterGain = gain;
    if (impl_->mix) {
        impl_->mix->masterGain = gain;
    }
    return {};
}

Result<std::optional<Completion>> AudioMiniaudio::PollCompletion() {
    std::lock_guard<std::mutex> guard(impl_->lock);
    if (!impl_->mix) {
        return std::optional<Completion>{};
    }
    return impl_->mix->NextCompletion();
}

Status AudioMiniaudio::Render(float *out, std::size_t frames) {
    std::lock_guard<std::mutex> guard(impl_->lock);
    if (!impl_->open || impl_->output != AudioOutput::kManualClock) {
        return Error::kWrongState;
    }
    impl_->mix->Mix(out, frames);
    return {};
}

bool AudioMiniaudio::Silent() const {
    std::lock_guard<std::mutex> guard(impl_->lock);
    return impl_->silent;
}

std::unique_ptr<Audio> makeAudio() { return std::make_unique<AudioMiniaudio>(); }

} // namespace sfcop::platform
