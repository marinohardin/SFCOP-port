#include "hle/ds3d/SoundBuffer.h"

#include "hle/ds3d/Buffer3D.h"
#include "hle/ds3d/DirectSound.h"

#include <algorithm>
#include <cstring>

namespace sfcop::hle::ds3d {
namespace {

// The size the primary buffer reports: 100 ms of the output, 16-bit stereo.
constexpr std::uint32_t kPrimaryBytes = kOutputRate / 10 * 4;
// The lowest clip rate the Audio component takes; a slower buffer plays its clip at its own rate.
constexpr std::uint32_t kLowestClipRate = 1000;

platform::ClipFormat clipFormat(const WaveFormat &format) {
    platform::ClipFormat clip;
    clip.format =
        format.bitsPerSample == 8 ? platform::SampleFormat::kPcm8 : platform::SampleFormat::kPcm16;
    clip.channels = static_cast<std::uint8_t>(format.channels);
    clip.sampleRate = (std::max)(format.samplesPerSec, kLowestClipRate);
    return clip;
}

} // namespace

// The bytes and the clip, shared by a buffer and its duplicates.
struct SoundBuffer::Storage {
    Storage(platform::Audio &audio, Memory &memory) : audio(audio), memory(memory) {}
    ~Storage() {
        if (clip.Valid()) {
            (void)audio.ReleaseClip(clip);
        }
        if (bytes != nullptr) {
            memory.Free(bytes);
        }
    }
    Storage(const Storage &) = delete;
    Storage &operator=(const Storage &) = delete;

    platform::Audio &audio;
    Memory &memory;
    std::uint8_t *bytes = nullptr;
    std::uint32_t clipBytes = 0; // whole frames
    platform::ClipId clip;
};

SoundBuffer::SoundBuffer(DirectSound &device, std::uint32_t flags)
    : device_(device), audio_(device.AudioOut()), primary_(true), flags_(flags),
      bytes_(kPrimaryBytes) {}

SoundBuffer::SoundBuffer(DirectSound &device, const BufferDesc &desc)
    : device_(device), audio_(device.AudioOut()), primary_(false), flags_(desc.flags),
      format_(desc.format), bytes_(desc.bufferBytes), frequency_(desc.format.samplesPerSec) {}

SoundBuffer::~SoundBuffer() {
    buffer3D_.reset();
    if (voice_.Valid()) {
        (void)audio_.DestroyVoice(voice_);
    }
}

HResult SoundBuffer::Init(const SoundBuffer *source) {
    if (primary_) {
        return kDsOk;
    }
    if (source != nullptr) {
        storage_ = source->storage_;
        volume_ = source->volume_;
        pan_ = source->pan_;
        frequency_ = source->frequency_;
    } else {
        auto storage = std::make_shared<Storage>(audio_, device_.BufferMemory());
        storage->bytes = device_.BufferMemory().Allocate(bytes_);
        if (storage->bytes == nullptr) {
            return kDsErrOutOfMemory;
        }
        if (format_.bitsPerSample == 8) {
            std::memset(storage->bytes, 0x80, bytes_);
        }
        storage->clipBytes = frames() * format_.blockAlign;
        platform::Result<platform::ClipId> clip =
            audio_.LoadClip(clipFormat(format_), storage->bytes, storage->clipBytes);
        if (!clip.Ok()) {
            return kDsErrOutOfMemory;
        }
        storage->clip = clip.Value();
        storage_ = std::move(storage);
    }
    platform::Result<platform::VoiceId> voice = audio_.CreateVoice(storage_->clip);
    if (!voice.Ok()) {
        return kDsErrOutOfMemory;
    }
    voice_ = voice.Value();
    if (volume_ != 0) {
        (void)audio_.SetGain(voice_, gainFromVolume(volume_));
    }
    if (pan_ != 0) {
        (void)audio_.SetPan(voice_, panFromDs(pan_));
    }
    if (frequency_ != clipFormat(format_).sampleRate) {
        (void)audio_.SetRate(voice_, frequency_);
    }
    if ((flags_ & kDsbCapsCtrl3D) != 0) {
        buffer3D_ = std::make_unique<Buffer3D>(audio_, voice_);
    }
    return kDsOk;
}

std::uint32_t SoundBuffer::frames() const { return bytes_ / format_.blockAlign; }

std::uint32_t SoundBuffer::leadBytes() const {
    return (std::max)(frequency_ / 100, 1u) * format_.blockAlign;
}

BufferCaps SoundBuffer::GetCaps() const {
    BufferCaps caps;
    caps.flags = flags_;
    if ((flags_ & (kDsbCapsLocHardware | kDsbCapsLocSoftware)) == 0) {
        caps.flags |= kDsbCapsLocSoftware;
    }
    caps.bufferBytes = bytes_;
    return caps;
}

HResult SoundBuffer::SetFormat(const WaveFormat &format) {
    if (!primary_) {
        return kDsErrInvalidCall;
    }
    if (device_.CooperativeLevel() < kDsSclPriority) {
        return kDsErrPrioLevelNeeded;
    }
    if (!validFormat(format)) {
        return kDsErrBadFormat;
    }
    format_ = format;
    return kDsOk;
}

HResult SoundBuffer::GetCurrentPosition(std::uint32_t *play, std::uint32_t *write) {
    std::uint32_t playAt = 0;
    std::uint32_t writeAt = 0;
    if (!primary_) {
        platform::Result<platform::VoiceStatus> status = audio_.GetStatus(voice_);
        if (!status.Ok()) {
            return kDsErrInvalidCall;
        }
        std::uint64_t frame = status.Value().frame;
        playAt = frame >= frames() ? 0 : static_cast<std::uint32_t>(frame) * format_.blockAlign;
        writeAt = playAt;
        if (status.Value().state == platform::VoiceState::kPlaying) {
            writeAt = (playAt + leadBytes()) % (frames() * format_.blockAlign);
        }
    }
    if (play != nullptr) {
        *play = playAt;
    }
    if (write != nullptr) {
        *write = writeAt;
    }
    return kDsOk;
}

HResult SoundBuffer::SetCurrentPosition(std::uint32_t offset) {
    if (primary_) {
        return kDsErrInvalidCall;
    }
    if (offset >= bytes_) {
        return kDsErrInvalidParam;
    }
    (void)audio_.SetPosition(voice_, offset / format_.blockAlign);
    return kDsOk;
}

HResult SoundBuffer::GetStatus(std::uint32_t *status) {
    if (status == nullptr) {
        return kDsErrInvalidParam;
    }
    device_.DrainCompletions();
    bool playing = primaryPlaying_;
    if (!primary_) {
        platform::Result<platform::VoiceStatus> voice = audio_.GetStatus(voice_);
        if (!voice.Ok()) {
            return kDsErrInvalidCall;
        }
        playing = voice.Value().state == platform::VoiceState::kPlaying;
    }
    *status = 0;
    if (playing) {
        *status = kDsbStatusPlaying | (looping_ ? kDsbStatusLooping : 0);
    }
    return kDsOk;
}

HResult SoundBuffer::Play(std::uint32_t flags) {
    if (primary_) {
        // The primary buffer always plays looping: the output runs while anything plays.
        if ((flags & kDsbPlayLooping) == 0) {
            return kDsErrInvalidParam;
        }
        primaryPlaying_ = true;
        looping_ = true;
        return kDsOk;
    }
    platform::Result<platform::VoiceStatus> status = audio_.GetStatus(voice_);
    if (!status.Ok()) {
        return kDsErrInvalidCall;
    }
    if (status.Value().state != platform::VoiceState::kPlaying &&
        status.Value().frame >= frames()) {
        (void)audio_.SetPosition(voice_, 0);
    }
    looping_ = (flags & kDsbPlayLooping) != 0;
    (void)audio_.Play(voice_, looping_);
    return kDsOk;
}

HResult SoundBuffer::Stop() {
    if (primary_) {
        primaryPlaying_ = false;
        return kDsOk;
    }
    (void)audio_.Stop(voice_);
    return kDsOk;
}

HResult SoundBuffer::Lock(std::uint32_t offset, std::uint32_t bytes, std::uint32_t flags,
                          LockedRange *range) {
    if (primary_) {
        return kDsErrPrioLevelNeeded;
    }
    if (range == nullptr) {
        return kDsErrInvalidParam;
    }
    if ((flags & kDsbLockFromWriteCursor) != 0) {
        std::uint32_t play = 0;
        HResult got = GetCurrentPosition(&play, &offset);
        if (got != kDsOk) {
            return got;
        }
    }
    if ((flags & kDsbLockEntireBuffer) != 0) {
        bytes = bytes_;
    }
    if (offset >= bytes_ || bytes == 0 || bytes > bytes_) {
        return kDsErrInvalidParam;
    }
    *range = LockedRange{};
    range->first = storage_->bytes + offset;
    range->firstBytes = (std::min)(bytes, bytes_ - offset);
    if (range->firstBytes < bytes) {
        range->second = storage_->bytes;
        range->secondBytes = bytes - range->firstBytes;
    }
    return kDsOk;
}

HResult SoundBuffer::Unlock(const LockedRange &range) {
    if (primary_) {
        return kDsErrPrioLevelNeeded;
    }
    const std::uint8_t *begin = storage_->bytes;
    const std::uint8_t *end = begin + bytes_;
    auto inside = [&](const std::uint8_t *piece, std::uint32_t bytes) {
        return bytes == 0 || (piece != nullptr && piece >= begin && piece <= end &&
                              bytes <= static_cast<std::uint32_t>(end - piece));
    };
    if (!inside(range.first, range.firstBytes) || !inside(range.second, range.secondBytes)) {
        return kDsErrInvalidParam;
    }
    auto copy = [&](const std::uint8_t *piece, std::uint32_t bytes) {
        std::uint32_t offset = static_cast<std::uint32_t>(piece - begin);
        if (bytes == 0 || offset >= storage_->clipBytes) {
            return;
        }
        std::uint32_t count = (std::min)(bytes, storage_->clipBytes - offset);
        (void)audio_.WriteClip(storage_->clip, offset, piece, count);
    };
    copy(range.first, range.firstBytes);
    copy(range.second, range.secondBytes);
    return kDsOk;
}

HResult SoundBuffer::GetVolume(std::int32_t *volume) const {
    if ((flags_ & kDsbCapsCtrlVolume) == 0) {
        return kDsErrControlUnavail;
    }
    if (volume == nullptr) {
        return kDsErrInvalidParam;
    }
    *volume = volume_;
    return kDsOk;
}

HResult SoundBuffer::SetVolume(std::int32_t volume) {
    if ((flags_ & kDsbCapsCtrlVolume) == 0) {
        return kDsErrControlUnavail;
    }
    if (volume < kDsbVolumeMin || volume > 0) {
        return kDsErrInvalidParam;
    }
    volume_ = volume;
    if (primary_) {
        (void)audio_.SetMasterGain(gainFromVolume(volume));
    } else {
        (void)audio_.SetGain(voice_, gainFromVolume(volume));
    }
    return kDsOk;
}

HResult SoundBuffer::GetPan(std::int32_t *pan) const {
    if ((flags_ & kDsbCapsCtrlPan) == 0) {
        return kDsErrControlUnavail;
    }
    if (pan == nullptr) {
        return kDsErrInvalidParam;
    }
    *pan = pan_;
    return kDsOk;
}

HResult SoundBuffer::SetPan(std::int32_t pan) {
    if ((flags_ & kDsbCapsCtrlPan) == 0) {
        return kDsErrControlUnavail;
    }
    if (pan < kDsbPanLeft || pan > kDsbPanRight) {
        return kDsErrInvalidParam;
    }
    pan_ = pan;
    // The output has no pan of its own: the primary buffer's is kept and reads back.
    if (!primary_) {
        (void)audio_.SetPan(voice_, panFromDs(pan));
    }
    return kDsOk;
}

HResult SoundBuffer::GetFrequency(std::uint32_t *hz) const {
    if (primary_ || (flags_ & kDsbCapsCtrlFrequency) == 0) {
        return kDsErrControlUnavail;
    }
    if (hz == nullptr) {
        return kDsErrInvalidParam;
    }
    *hz = frequency_;
    return kDsOk;
}

HResult SoundBuffer::SetFrequency(std::uint32_t hz) {
    if (primary_ || (flags_ & kDsbCapsCtrlFrequency) == 0) {
        return kDsErrControlUnavail;
    }
    if (hz == kDsbFrequencyOriginal) {
        hz = format_.samplesPerSec;
    }
    if (hz < kDsbFrequencyMin || hz > kDsbFrequencyMax) {
        return kDsErrInvalidParam;
    }
    frequency_ = hz;
    (void)audio_.SetRate(voice_, hz);
    return kDsOk;
}

} // namespace sfcop::hle::ds3d
