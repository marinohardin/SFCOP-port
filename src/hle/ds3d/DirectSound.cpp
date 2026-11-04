#include "hle/ds3d/DirectSound.h"

#include "hle/ds3d/Buffer3D.h"
#include "hle/ds3d/Listener.h"
#include "hle/ds3d/SoundBuffer.h"

#include <algorithm>
#include <cstdlib>

namespace sfcop::hle::ds3d {
namespace {

// DSCAPS_PRIMARYMONO, _PRIMARYSTEREO, _PRIMARY8BIT, _PRIMARY16BIT, _CONTINUOUSRATE,
// _CERTIFIED, _SECONDARYMONO, _SECONDARYSTEREO, _SECONDARY8BIT, _SECONDARY16BIT.
constexpr std::uint32_t kDeviceCaps = 0x00000F5F;
constexpr std::uint32_t kMixingBuffers = 64;

class HostMemory final : public Memory {
public:
    std::uint8_t *Allocate(std::size_t bytes) override {
        return static_cast<std::uint8_t *>(std::calloc(bytes, 1));
    }
    void Free(std::uint8_t *memory) override { std::free(memory); }
};

} // namespace

Memory &hostMemory() {
    static HostMemory memory;
    return memory;
}

bool validFormat(const WaveFormat &f) {
    return f.formatTag == kWaveFormatPcm && (f.channels == 1 || f.channels == 2) &&
           (f.bitsPerSample == 8 || f.bitsPerSample == 16) &&
           f.samplesPerSec >= kDsbFrequencyMin && f.samplesPerSec <= kDsbFrequencyMax &&
           f.blockAlign == f.channels * f.bitsPerSample / 8;
}

DirectSound::DirectSound(platform::Audio &audio, Memory &memory) : audio_(audio), memory_(memory) {}

DirectSound::~DirectSound() {
    buffers_.clear();
    listener_.reset();
    if (open_) {
        (void)audio_.Close();
    }
}

HResult DirectSound::Open() {
    if (open_) {
        return kDsOk;
    }
    platform::OutputDesc desc;
    desc.sampleRate = kOutputRate;
    desc.channels = 2;
    platform::Status opened = audio_.Open(desc);
    if (!opened.Ok() && opened.Code() != platform::Error::kWrongState) {
        return kDsErrNoDriver;
    }
    open_ = true;
    return kDsOk;
}

HResult DirectSound::SetCooperativeLevel(bool hasWindow, std::uint32_t level) {
    if (!hasWindow || level < kDsSclNormal || level > kDsSclWritePrimary) {
        return kDsErrInvalidParam;
    }
    level_ = level;
    return kDsOk;
}

Caps DirectSound::GetCaps() const {
    Caps caps;
    caps.flags = kDeviceCaps;
    caps.minSecondarySampleRate = kDsbFrequencyMin;
    caps.maxSecondarySampleRate = kDsbFrequencyMax;
    caps.primaryBuffers = 1;
    caps.maxHwMixingAllBuffers = kMixingBuffers;
    caps.maxHwMixingStaticBuffers = kMixingBuffers;
    caps.maxHwMixingStreamingBuffers = kMixingBuffers;
    caps.freeHwMixingAllBuffers = kMixingBuffers;
    caps.freeHwMixingStaticBuffers = kMixingBuffers;
    caps.freeHwMixingStreamingBuffers = kMixingBuffers;
    return caps;
}

SoundBuffer *DirectSound::add(std::unique_ptr<SoundBuffer> buffer) {
    buffers_.push_back(std::move(buffer));
    return buffers_.back().get();
}

HResult DirectSound::CreateSoundBuffer(const BufferDesc &desc, SoundBuffer **out) {
    if (out == nullptr) {
        return kDsErrInvalidParam;
    }
    *out = nullptr;
    if (!open_) {
        return kDsErrInvalidCall;
    }
    if ((desc.flags & kDsbCapsPrimaryBuffer) != 0) {
        if (desc.bufferBytes != 0 || desc.hasFormat ||
            (desc.flags & (kDsbCapsCtrlFrequency | kDsbCapsStatic)) != 0) {
            return kDsErrInvalidParam;
        }
        if (primary_ == nullptr) {
            primary_ = add(std::make_unique<SoundBuffer>(*this, desc.flags));
        }
        if ((desc.flags & kDsbCapsCtrl3D) != 0 && !listener_) {
            listener_ = std::make_unique<Listener>(audio_, *this);
        }
        *out = primary_;
        return kDsOk;
    }
    if (!desc.hasFormat || desc.bufferBytes < kDsbSizeMin || desc.bufferBytes > kDsbSizeMax) {
        return kDsErrInvalidParam;
    }
    if (!validFormat(desc.format)) {
        return kDsErrBadFormat;
    }
    if ((desc.flags & kDsbCapsCtrl3D) != 0 && desc.format.channels != 1) {
        return kDsErrInvalidParam;
    }
    auto buffer = std::make_unique<SoundBuffer>(*this, desc);
    HResult made = buffer->Init();
    if (made != kDsOk) {
        return made;
    }
    *out = add(std::move(buffer));
    return kDsOk;
}

HResult DirectSound::DuplicateSoundBuffer(SoundBuffer &source, SoundBuffer **out) {
    if (out == nullptr) {
        return kDsErrInvalidParam;
    }
    *out = nullptr;
    if (source.IsPrimary()) {
        return kDsErrInvalidCall;
    }
    BufferDesc desc;
    desc.flags = source.Flags();
    desc.bufferBytes = source.GetCaps().bufferBytes;
    desc.hasFormat = true;
    desc.format = source.GetFormat();
    auto buffer = std::make_unique<SoundBuffer>(*this, desc);
    HResult made = buffer->Init(&source);
    if (made != kDsOk) {
        return made;
    }
    *out = add(std::move(buffer));
    return kDsOk;
}

void DirectSound::ReleaseBuffer(SoundBuffer *buffer) {
    if (buffer == primary_) {
        primary_ = nullptr;
    }
    auto found =
        std::find_if(buffers_.begin(), buffers_.end(),
                     [&](const std::unique_ptr<SoundBuffer> &b) { return b.get() == buffer; });
    if (found != buffers_.end()) {
        buffers_.erase(found);
    }
}

HResult DirectSound::SetSpeakerConfig(std::uint32_t config) {
    speakerConfig_ = config;
    return kDsOk;
}

void DirectSound::CommitDeferred() {
    if (listener_) {
        listener_->ApplyPending();
    }
    for (const std::unique_ptr<SoundBuffer> &buffer : buffers_) {
        if (Buffer3D *b3d = buffer->Get3D()) {
            b3d->ApplyPending();
        }
    }
}

void DirectSound::DrainCompletions() {
    for (;;) {
        platform::Result<std::optional<platform::Completion>> next = audio_.PollCompletion();
        if (!next.Ok() || !next.Value().has_value()) {
            return;
        }
    }
}

} // namespace sfcop::hle::ds3d
