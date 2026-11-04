// The stub backend: compiles on every platform and supports nothing.
#include "platform/Audio/Audio.h"

namespace sfcop::platform {
namespace {

class AudioStub final : public Audio {
public:
    Status Open(const OutputDesc &) override { return Error::kUnsupported; }
    Status Close() override { return Error::kUnsupported; }
    Result<ClipId> LoadClip(const ClipFormat &, const void *, std::size_t) override {
        return Error::kUnsupported;
    }
    Status WriteClip(ClipId, std::size_t, const void *, std::size_t) override {
        return Error::kUnsupported;
    }
    Status ReleaseClip(ClipId) override { return Error::kUnsupported; }
    Result<VoiceId> CreateVoice(ClipId) override { return Error::kUnsupported; }
    Status DestroyVoice(VoiceId) override { return Error::kUnsupported; }
    Status Play(VoiceId, bool) override { return Error::kUnsupported; }
    Status Stop(VoiceId) override { return Error::kUnsupported; }
    Status SetPosition(VoiceId, std::uint64_t) override { return Error::kUnsupported; }
    Result<VoiceStatus> GetStatus(VoiceId) override { return Error::kUnsupported; }
    Status SetGain(VoiceId, float) override { return Error::kUnsupported; }
    Status SetPan(VoiceId, float) override { return Error::kUnsupported; }
    Status SetRate(VoiceId, std::uint32_t) override { return Error::kUnsupported; }
    Status SetVoice3D(VoiceId, const Voice3D &) override { return Error::kUnsupported; }
    Status SetListener(const Listener &) override { return Error::kUnsupported; }
    Result<StreamId> OpenStream(const ClipFormat &) override { return Error::kUnsupported; }
    Status QueueBuffer(StreamId, const void *, std::size_t) override {
        return Error::kUnsupported;
    }
    Result<std::size_t> QueuedBuffers(StreamId) override { return Error::kUnsupported; }
    Status PlayStream(StreamId) override { return Error::kUnsupported; }
    Status StopStream(StreamId) override { return Error::kUnsupported; }
    Status FlushStream(StreamId) override { return Error::kUnsupported; }
    Status SetStreamGain(StreamId, float) override { return Error::kUnsupported; }
    Status SetStreamPan(StreamId, float) override { return Error::kUnsupported; }
    Status CloseStream(StreamId) override { return Error::kUnsupported; }
    Status SetMasterGain(float) override { return Error::kUnsupported; }
    Result<std::optional<Completion>> PollCompletion() override { return Error::kUnsupported; }
};

} // namespace

std::unique_ptr<Audio> makeAudioStub() { return std::make_unique<AudioStub>(); }

} // namespace sfcop::platform
