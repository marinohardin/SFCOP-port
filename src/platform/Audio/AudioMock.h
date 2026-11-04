// An Audio for tests: records each call and returns the values the test programs. Every Status
// method returns `failWith` (success while it is kNone); the methods that return a value return
// their `...Result` member. The last 3D parameters and listener are kept whole. PollCompletion()
// hands out `completions` front first, as the mixer thread would have queued them.
#pragma once

#include "platform/Audio/Audio.h"
#include "platform/Core/MockLog.h"

#include <deque>

namespace sfcop::platform {

class AudioMock final : public Audio {
public:
    MockLog log;

    std::deque<Completion> completions;

    Error failWith = Error::kNone;
    ClipFormat lastClipFormat;
    Voice3D lastVoice3D;
    Listener lastListener;

    Result<ClipId> loadClipResult = ClipId{1};
    Result<VoiceId> createVoiceResult = VoiceId{1};
    Result<VoiceStatus> statusResult = VoiceStatus{};
    Result<StreamId> openStreamResult = StreamId{1};
    Result<std::size_t> queuedBuffersResult = std::size_t{0};

    Status Open(const OutputDesc &desc) override {
        log.Record("Open", desc.sampleRate, desc.channels);
        return status();
    }
    Status Close() override {
        log.Record("Close");
        return status();
    }
    Result<ClipId> LoadClip(const ClipFormat &format, const void *, std::size_t bytes) override {
        log.Record("LoadClip", format.format, format.channels, format.sampleRate, bytes);
        lastClipFormat = format;
        return loadClipResult;
    }
    Status WriteClip(ClipId clip, std::size_t offsetBytes, const void *,
                     std::size_t bytes) override {
        log.Record("WriteClip", clip, offsetBytes, bytes);
        return status();
    }
    Status ReleaseClip(ClipId clip) override {
        log.Record("ReleaseClip", clip);
        return status();
    }
    Result<VoiceId> CreateVoice(ClipId clip) override {
        log.Record("CreateVoice", clip);
        return createVoiceResult;
    }
    Status DestroyVoice(VoiceId voice) override {
        log.Record("DestroyVoice", voice);
        return status();
    }
    Status Play(VoiceId voice, bool loop) override {
        log.Record("Play", voice, loop);
        return status();
    }
    Status Stop(VoiceId voice) override {
        log.Record("Stop", voice);
        return status();
    }
    Status SetPosition(VoiceId voice, std::uint64_t frame) override {
        log.Record("SetPosition", voice, frame);
        return status();
    }
    Result<VoiceStatus> GetStatus(VoiceId voice) override {
        log.Record("GetStatus", voice);
        return statusResult;
    }
    Status SetGain(VoiceId voice, float gain) override {
        log.Record("SetGain", voice, gain);
        return status();
    }
    Status SetPan(VoiceId voice, float pan) override {
        log.Record("SetPan", voice, pan);
        return status();
    }
    Status SetRate(VoiceId voice, std::uint32_t hz) override {
        log.Record("SetRate", voice, hz);
        return status();
    }
    Status SetVoice3D(VoiceId voice, const Voice3D &params) override {
        log.Record("SetVoice3D", voice, params.space);
        lastVoice3D = params;
        return status();
    }
    Status SetListener(const Listener &listener) override {
        log.Record("SetListener");
        lastListener = listener;
        return status();
    }
    Result<StreamId> OpenStream(const ClipFormat &format) override {
        log.Record("OpenStream", format.format, format.channels, format.sampleRate);
        lastClipFormat = format;
        return openStreamResult;
    }
    Status QueueBuffer(StreamId stream, const void *, std::size_t bytes) override {
        log.Record("QueueBuffer", stream, bytes);
        return status();
    }
    Result<std::size_t> QueuedBuffers(StreamId stream) override {
        log.Record("QueuedBuffers", stream);
        return queuedBuffersResult;
    }
    Status PlayStream(StreamId stream) override {
        log.Record("PlayStream", stream);
        return status();
    }
    Status StopStream(StreamId stream) override {
        log.Record("StopStream", stream);
        return status();
    }
    Status FlushStream(StreamId stream) override {
        log.Record("FlushStream", stream);
        return status();
    }
    Status SetStreamGain(StreamId stream, float gain) override {
        log.Record("SetStreamGain", stream, gain);
        return status();
    }
    Status SetStreamPan(StreamId stream, float pan) override {
        log.Record("SetStreamPan", stream, pan);
        return status();
    }
    Status CloseStream(StreamId stream) override {
        log.Record("CloseStream", stream);
        return status();
    }
    Status SetMasterGain(float gain) override {
        log.Record("SetMasterGain", gain);
        return status();
    }
    Result<std::optional<Completion>> PollCompletion() override {
        log.Record("PollCompletion");
        if (failWith != Error::kNone) {
            return failWith;
        }
        if (completions.empty()) {
            return std::optional<Completion>{};
        }
        Completion next = completions.front();
        completions.pop_front();
        return std::optional<Completion>{next};
    }

private:
    Status status() const {
        if (failWith != Error::kNone) {
            return failWith;
        }
        return {};
    }
};

} // namespace sfcop::platform
