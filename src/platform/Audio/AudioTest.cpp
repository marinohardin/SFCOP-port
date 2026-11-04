#include "platform/Audio/Audio.h"
#include "platform/Audio/AudioMock.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace sfcop::platform {
namespace {

// A caller written against the interface only, as a positioned sound effect is: loads a clip,
// makes a voice, places it in the world and plays it once.
Result<VoiceId> playAt(Audio &audio, const std::vector<std::uint8_t> &pcm, Vector3 where) {
    ClipFormat format;
    format.format = SampleFormat::kPcm16;
    format.channels = 1;
    format.sampleRate = 22050;
    Result<ClipId> clip = audio.LoadClip(format, pcm.data(), pcm.size());
    if (!clip.Ok()) {
        return clip.Code();
    }
    Result<VoiceId> voice = audio.CreateVoice(clip.Value());
    if (!voice.Ok()) {
        (void)audio.ReleaseClip(clip.Value());
        return voice.Code();
    }
    Voice3D params;
    params.space = VoiceSpace::kWorld;
    params.position = where;
    params.minDistance = 10.0f;
    Status placed = audio.SetVoice3D(voice.Value(), params);
    if (!placed.Ok()) {
        return placed.Code();
    }
    Status played = audio.Play(voice.Value(), false);
    if (!played.Ok()) {
        return played.Code();
    }
    return voice.Value();
}

} // namespace

TEST_CASE("a caller plays a positioned clip once through the audio interface") {
    AudioMock mock;
    mock.loadClipResult = ClipId{3};
    mock.createVoiceResult = VoiceId{8};
    std::vector<std::uint8_t> pcm(441 * 2, 0);
    Result<VoiceId> voice = playAt(mock, pcm, Vector3{1.0f, 0.0f, 25.0f});
    REQUIRE(voice.Ok());
    CHECK(voice.Value() == VoiceId{8});
    CHECK(mock.lastVoice3D.position.z == 25.0f);
    CHECK(mock.lastVoice3D.minDistance == 10.0f);
    CHECK(mock.lastVoice3D.coneOutsideGain == 1.0f);
    CHECK(mock.log.Calls() == std::vector<std::string>{
                                  "LoadClip(1, 1, 22050, 882)",
                                  "CreateVoice(3)",
                                  "SetVoice3D(8, 1)",
                                  "Play(8, false)",
                              });
}

TEST_CASE("running out of voices releases the clip and reaches the caller") {
    AudioMock mock;
    mock.loadClipResult = ClipId{4};
    mock.createVoiceResult = Error::kOutOfResources;
    std::vector<std::uint8_t> pcm(4, 0);
    CHECK(playAt(mock, pcm, Vector3{}).Code() == Error::kOutOfResources);
    CHECK(mock.log.Calls() == std::vector<std::string>{
                                  "LoadClip(1, 1, 22050, 4)",
                                  "CreateVoice(4)",
                                  "ReleaseClip(4)",
                              });
}

TEST_CASE("the audio mock reports a stream's queue as programmed") {
    AudioMock mock;
    Audio &audio = mock;
    ClipFormat adpcm;
    adpcm.format = SampleFormat::kImaAdpcm;
    adpcm.adpcmBlockBytes = 512;
    StreamId stream = audio.OpenStream(adpcm).Value();
    std::uint8_t block[512] = {};
    CHECK(audio.QueueBuffer(stream, block, sizeof block).Ok());
    mock.queuedBuffersResult = std::size_t{1};
    CHECK(audio.QueuedBuffers(stream).Value() == 1);
    mock.failWith = Error::kInvalidHandle;
    CHECK(audio.PlayStream(stream).Code() == Error::kInvalidHandle);
    CHECK(mock.lastClipFormat.adpcmBlockBytes == 512);
}

TEST_CASE("a service tick drains ended voices and finished buffers from the caller's thread") {
    AudioMock mock;
    Completion ended;
    ended.kind = CompletionKind::kVoiceEnded;
    ended.voice = VoiceId{8};
    Completion buffer;
    buffer.kind = CompletionKind::kStreamBufferDone;
    buffer.stream = StreamId{2};
    mock.completions.push_back(ended);
    mock.completions.push_back(buffer);

    // As the Miles HLE's service tick: run end-of-sample handling for each ended voice and count
    // the stream buffers that can be refilled.
    std::vector<VoiceId> endedVoices;
    int refills = 0;
    Audio &audio = mock;
    for (;;) {
        Result<std::optional<Completion>> next = audio.PollCompletion();
        REQUIRE(next.Ok());
        if (!next.Value()) {
            break;
        }
        if (next.Value()->kind == CompletionKind::kVoiceEnded) {
            endedVoices.push_back(next.Value()->voice);
        } else {
            ++refills;
        }
    }
    CHECK(endedVoices == std::vector<VoiceId>{VoiceId{8}});
    CHECK(refills == 1);
    CHECK(mock.log.Count("PollCompletion") == 3);
}

TEST_CASE("every method of the audio stub reports unsupported") {
    std::unique_ptr<Audio> audio = makeAudioStub();
    ClipId clip{1};
    VoiceId voice{1};
    StreamId stream{1};
    std::uint8_t data[4] = {};
    CHECK(audio->Open(OutputDesc{}).Code() == Error::kUnsupported);
    CHECK(audio->Close().Code() == Error::kUnsupported);
    CHECK(audio->LoadClip(ClipFormat{}, data, 4).Code() == Error::kUnsupported);
    CHECK(audio->WriteClip(clip, 0, data, 4).Code() == Error::kUnsupported);
    CHECK(audio->ReleaseClip(clip).Code() == Error::kUnsupported);
    CHECK(audio->CreateVoice(clip).Code() == Error::kUnsupported);
    CHECK(audio->DestroyVoice(voice).Code() == Error::kUnsupported);
    CHECK(audio->Play(voice, true).Code() == Error::kUnsupported);
    CHECK(audio->Stop(voice).Code() == Error::kUnsupported);
    CHECK(audio->SetPosition(voice, 0).Code() == Error::kUnsupported);
    CHECK(audio->GetStatus(voice).Code() == Error::kUnsupported);
    CHECK(audio->SetGain(voice, 0.5f).Code() == Error::kUnsupported);
    CHECK(audio->SetPan(voice, 0.0f).Code() == Error::kUnsupported);
    CHECK(audio->SetRate(voice, 22050).Code() == Error::kUnsupported);
    CHECK(audio->SetVoice3D(voice, Voice3D{}).Code() == Error::kUnsupported);
    CHECK(audio->SetListener(Listener{}).Code() == Error::kUnsupported);
    CHECK(audio->OpenStream(ClipFormat{}).Code() == Error::kUnsupported);
    CHECK(audio->QueueBuffer(stream, data, 4).Code() == Error::kUnsupported);
    CHECK(audio->QueuedBuffers(stream).Code() == Error::kUnsupported);
    CHECK(audio->PlayStream(stream).Code() == Error::kUnsupported);
    CHECK(audio->StopStream(stream).Code() == Error::kUnsupported);
    CHECK(audio->FlushStream(stream).Code() == Error::kUnsupported);
    CHECK(audio->SetStreamGain(stream, 1.0f).Code() == Error::kUnsupported);
    CHECK(audio->SetStreamPan(stream, 0.0f).Code() == Error::kUnsupported);
    CHECK(audio->CloseStream(stream).Code() == Error::kUnsupported);
    CHECK(audio->SetMasterGain(1.0f).Code() == Error::kUnsupported);
    CHECK(audio->PollCompletion().Code() == Error::kUnsupported);
}

} // namespace sfcop::platform
