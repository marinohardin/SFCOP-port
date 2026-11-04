#include "hle/ds3d/SoundBuffer.h"

#include "hle/ds3d/DirectSound.h"

#include "platform/Audio/AudioMock.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace sfcop::hle::ds3d {
namespace {

using platform::AudioMock;
using platform::VoiceState;
using platform::VoiceStatus;
using Calls = std::vector<std::string>;

constexpr std::uint32_t kControls = kDsbCapsCtrlVolume | kDsbCapsCtrlPan | kDsbCapsCtrlFrequency;

// A 16-bit stereo 44.1 kHz buffer of 4000 bytes: 1000 frames of 4 bytes; 10 ms is 1764 bytes.
struct Rig {
    AudioMock audio;
    DirectSound device{audio, hostMemory()};
    SoundBuffer *buffer = nullptr;

    explicit Rig(std::uint32_t flags = kControls, std::uint16_t bits = 16) {
        REQUIRE(device.Open() == kDsOk);
        BufferDesc desc;
        desc.flags = flags;
        desc.bufferBytes = 4000;
        desc.hasFormat = true;
        desc.format.channels = 2;
        desc.format.bitsPerSample = bits;
        desc.format.samplesPerSec = 44100;
        desc.format.blockAlign = static_cast<std::uint16_t>(2 * bits / 8);
        desc.format.avgBytesPerSec = 44100u * desc.format.blockAlign;
        REQUIRE(device.CreateSoundBuffer(desc, &buffer) == kDsOk);
        audio.log.Clear();
    }
    void At(std::uint64_t frame, bool playing) {
        VoiceStatus status;
        status.state = playing ? VoiceState::kPlaying : VoiceState::kStopped;
        status.frame = frame;
        audio.statusResult = status;
    }
    SoundBuffer *Primary(std::uint32_t flags) {
        BufferDesc desc;
        desc.flags = kDsbCapsPrimaryBuffer | flags;
        SoundBuffer *main = nullptr;
        REQUIRE(device.CreateSoundBuffer(desc, &main) == kDsOk);
        audio.log.Clear();
        return main;
    }
};

} // namespace

TEST_CASE("Lock hands out the bytes asked for and Unlock copies them into the clip") {
    Rig rig;
    LockedRange range;
    CHECK(rig.buffer->Lock(100, 200, 0, &range) == kDsOk);
    REQUIRE(range.first != nullptr);
    CHECK(range.firstBytes == 200u);
    CHECK(range.second == nullptr);
    CHECK(range.secondBytes == 0u);
    CHECK(range.first[0] == 0); // 16-bit silence
    range.first[0] = 0x12;
    CHECK(rig.buffer->Unlock(range) == kDsOk);
    CHECK(rig.audio.log.Calls() == Calls{"WriteClip(1, 100, 200)"});
    LockedRange again;
    REQUIRE(rig.buffer->Lock(100, 4, 0, &again) == kDsOk);
    CHECK(again.first[0] == 0x12);
}

TEST_CASE("a lock past the end wraps to the start of the buffer") {
    Rig rig;
    LockedRange range;
    CHECK(rig.buffer->Lock(3900, 200, 0, &range) == kDsOk);
    CHECK(range.firstBytes == 100u);
    REQUIRE(range.second != nullptr);
    CHECK(range.secondBytes == 100u);
    CHECK(range.second + 3900 == range.first);
    CHECK(rig.buffer->Unlock(range) == kDsOk);
    CHECK(rig.audio.log.Calls() == Calls{"WriteClip(1, 3900, 100)", "WriteClip(1, 0, 100)"});
}

TEST_CASE("DSBLOCK_ENTIREBUFFER locks the whole buffer") {
    Rig rig;
    LockedRange range;
    CHECK(rig.buffer->Lock(0, 0, kDsbLockEntireBuffer, &range) == kDsOk);
    CHECK(range.firstBytes == 4000u);
    CHECK(range.second == nullptr);
}

TEST_CASE("DSBLOCK_FROMWRITECURSOR locks from 10 ms ahead of the play cursor") {
    Rig rig;
    rig.At(10, true);
    LockedRange range;
    CHECK(rig.buffer->Lock(0, 100, kDsbLockFromWriteCursor, &range) == kDsOk);
    LockedRange start;
    REQUIRE(rig.buffer->Lock(0, 4, 0, &start) == kDsOk);
    CHECK(range.first - start.first == 40 + 1764);
}

TEST_CASE("Lock refuses an offset past the end and an empty or oversized range") {
    Rig rig;
    LockedRange range;
    CHECK(rig.buffer->Lock(4000, 4, 0, &range) == kDsErrInvalidParam);
    CHECK(rig.buffer->Lock(0, 0, 0, &range) == kDsErrInvalidParam);
    CHECK(rig.buffer->Lock(0, 4001, 0, &range) == kDsErrInvalidParam);
    CHECK(rig.buffer->Lock(0, 4, 0, nullptr) == kDsErrInvalidParam);
}

TEST_CASE("Unlock refuses a range outside the buffer") {
    Rig rig;
    LockedRange range;
    REQUIRE(rig.buffer->Lock(0, 100, 0, &range) == kDsOk);
    LockedRange outside = range;
    outside.firstBytes = 4001;
    CHECK(rig.buffer->Unlock(outside) == kDsErrInvalidParam);
    std::uint8_t elsewhere[4] = {};
    LockedRange foreign;
    foreign.first = elsewhere;
    foreign.firstBytes = 4;
    CHECK(rig.buffer->Unlock(foreign) == kDsErrInvalidParam);
    CHECK(rig.audio.log.Calls().empty());
}

TEST_CASE("bytes past the last whole frame are kept but not played") {
    AudioMock audio;
    DirectSound device{audio, hostMemory()};
    REQUIRE(device.Open() == kDsOk);
    BufferDesc desc;
    desc.bufferBytes = 4002;
    desc.hasFormat = true;
    desc.format.channels = 2;
    desc.format.bitsPerSample = 16;
    desc.format.samplesPerSec = 44100;
    desc.format.blockAlign = 4;
    SoundBuffer *buffer = nullptr;
    REQUIRE(device.CreateSoundBuffer(desc, &buffer) == kDsOk);
    audio.log.Clear();
    LockedRange range;
    REQUIRE(buffer->Lock(3998, 4, 0, &range) == kDsOk);
    CHECK(range.firstBytes == 4u);
    CHECK(buffer->Unlock(range) == kDsOk);
    CHECK(audio.log.Calls() == Calls{"WriteClip(1, 3998, 2)"});
}

TEST_CASE("an 8-bit buffer starts at the unsigned silence value") {
    Rig rig(kControls, 8);
    LockedRange range;
    REQUIRE(rig.buffer->Lock(0, 0, kDsbLockEntireBuffer, &range) == kDsOk);
    CHECK(range.first[0] == 0x80);
    CHECK(range.first[3999] == 0x80);
}

TEST_CASE("GetCurrentPosition reports the play cursor and a write cursor 10 ms ahead") {
    Rig rig;
    std::uint32_t play = 1;
    std::uint32_t write = 1;
    rig.At(250, false);
    CHECK(rig.buffer->GetCurrentPosition(&play, &write) == kDsOk);
    CHECK(play == 1000u);
    CHECK(write == 1000u);
    rig.At(250, true);
    CHECK(rig.buffer->GetCurrentPosition(&play, &write) == kDsOk);
    CHECK(play == 1000u);
    CHECK(write == 2764u);
    rig.At(900, true);
    CHECK(rig.buffer->GetCurrentPosition(&play, &write) == kDsOk);
    CHECK(play == 3600u);
    CHECK(write == 1364u);
    rig.At(1000, false);
    CHECK(rig.buffer->GetCurrentPosition(&play, nullptr) == kDsOk);
    CHECK(play == 0u);
}

TEST_CASE("SetCurrentPosition moves the voice to the whole frame at the offset") {
    Rig rig;
    CHECK(rig.buffer->SetCurrentPosition(1002) == kDsOk);
    CHECK(rig.buffer->SetCurrentPosition(4000) == kDsErrInvalidParam);
    CHECK(rig.audio.log.Calls() == Calls{"SetPosition(1, 250)"});
}

TEST_CASE("Play plays once or looping and a buffer played to its end starts again") {
    Rig rig;
    rig.At(10, false);
    CHECK(rig.buffer->Play(0) == kDsOk);
    rig.At(1000, false);
    CHECK(rig.buffer->Play(kDsbPlayLooping) == kDsOk);
    CHECK(rig.audio.log.Calls() == Calls{"GetStatus(1)", "Play(1, false)", "GetStatus(1)",
                                         "SetPosition(1, 0)", "Play(1, true)"});
}

TEST_CASE("GetStatus reads the voice and drains the completion queue") {
    Rig rig;
    std::uint32_t status = 9;
    rig.At(0, false);
    CHECK(rig.buffer->GetStatus(&status) == kDsOk);
    CHECK(status == 0u);
    rig.At(0, true);
    REQUIRE(rig.buffer->Play(0) == kDsOk);
    CHECK(rig.buffer->GetStatus(&status) == kDsOk);
    CHECK(status == kDsbStatusPlaying);
    REQUIRE(rig.buffer->Play(kDsbPlayLooping) == kDsOk);
    rig.audio.completions.push_back(platform::Completion{});
    CHECK(rig.buffer->GetStatus(&status) == kDsOk);
    CHECK(status == (kDsbStatusPlaying | kDsbStatusLooping));
    CHECK(rig.audio.completions.empty());
    CHECK(rig.buffer->GetStatus(nullptr) == kDsErrInvalidParam);
}

TEST_CASE("Stop stops the voice") {
    Rig rig;
    CHECK(rig.buffer->Stop() == kDsOk);
    CHECK(rig.audio.log.Calls() == Calls{"Stop(1)"});
}

TEST_CASE("SetVolume sets the voice's gain from hundredths of a decibel") {
    Rig rig;
    CHECK(rig.buffer->SetVolume(-2000) == kDsOk);
    CHECK(rig.buffer->SetVolume(-10000) == kDsOk);
    CHECK(rig.buffer->SetVolume(0) == kDsOk);
    CHECK(rig.buffer->SetVolume(1) == kDsErrInvalidParam);
    CHECK(rig.buffer->SetVolume(-10001) == kDsErrInvalidParam);
    CHECK(rig.audio.log.Calls() == Calls{"SetGain(1, 0.1)", "SetGain(1, 0)", "SetGain(1, 1)"});
    std::int32_t volume = 5;
    CHECK(rig.buffer->GetVolume(&volume) == kDsOk);
    CHECK(volume == 0);
}

TEST_CASE("SetPan passes DirectSound's pan to the Audio scale") {
    Rig rig;
    CHECK(rig.buffer->SetPan(0) == kDsOk);
    CHECK(rig.buffer->SetPan(10001) == kDsErrInvalidParam);
    CHECK(rig.audio.log.Calls() == Calls{"SetPan(1, 0)"});
    // DirectSound's pan is hundredths of a decibel on the far side, the near side at unity; the
    // Audio component's scale is the same number over 10000.
    CHECK(panFromDs(2000) == doctest::Approx(0.2f).epsilon(1e-5));
    CHECK(panFromDs(-2000) == doctest::Approx(-0.2f).epsilon(1e-5));
    CHECK(panFromDs(-10000) == doctest::Approx(-1.0f).epsilon(1e-5));
    CHECK(panFromDs(10000) == doctest::Approx(1.0f).epsilon(1e-5));
    std::int32_t pan = 5;
    CHECK(rig.buffer->GetPan(&pan) == kDsOk);
    CHECK(pan == 0);
}

TEST_CASE("SetFrequency sets the voice's rate and 0 restores the format's") {
    Rig rig;
    CHECK(rig.buffer->SetFrequency(22050) == kDsOk);
    std::uint32_t hz = 0;
    CHECK(rig.buffer->GetFrequency(&hz) == kDsOk);
    CHECK(hz == 22050u);
    CHECK(rig.buffer->SetFrequency(kDsbFrequencyOriginal) == kDsOk);
    CHECK(rig.buffer->SetFrequency(99) == kDsErrInvalidParam);
    CHECK(rig.buffer->SetFrequency(100001) == kDsErrInvalidParam);
    CHECK(rig.audio.log.Calls() == Calls{"SetRate(1, 22050)", "SetRate(1, 44100)"});
    CHECK(rig.buffer->GetFrequency(&hz) == kDsOk);
    CHECK(hz == 44100u);
}

TEST_CASE("a control the buffer was not created with is unavailable") {
    Rig rig(0);
    std::int32_t value = 0;
    std::uint32_t hz = 0;
    CHECK(rig.buffer->SetVolume(-100) == kDsErrControlUnavail);
    CHECK(rig.buffer->GetVolume(&value) == kDsErrControlUnavail);
    CHECK(rig.buffer->SetPan(100) == kDsErrControlUnavail);
    CHECK(rig.buffer->GetPan(&value) == kDsErrControlUnavail);
    CHECK(rig.buffer->SetFrequency(22050) == kDsErrControlUnavail);
    CHECK(rig.buffer->GetFrequency(&hz) == kDsErrControlUnavail);
    CHECK(rig.audio.log.Calls().empty());
}

TEST_CASE("GetCaps reports the flags, in software, and the size") {
    Rig rig;
    BufferCaps caps = rig.buffer->GetCaps();
    CHECK(caps.flags == (kControls | kDsbCapsLocSoftware));
    CHECK(caps.bufferBytes == 4000u);
    CHECK(caps.unlockTransferRate == 0u);
    CHECK(caps.playCpuOverhead == 0u);
}

TEST_CASE("GetFormat returns the buffer's format and SetFormat refuses a secondary buffer") {
    Rig rig;
    WaveFormat format = rig.buffer->GetFormat();
    CHECK(format.channels == 2u);
    CHECK(format.bitsPerSample == 16u);
    CHECK(format.samplesPerSec == 44100u);
    CHECK(format.blockAlign == 4u);
    CHECK(rig.buffer->SetFormat(format) == kDsErrInvalidCall);
}

TEST_CASE("Restore succeeds and Initialize finds the buffer initialized") {
    Rig rig;
    CHECK(rig.buffer->Restore() == kDsOk);
    CHECK(rig.buffer->Initialize() == kDsErrAlreadyInitialized);
}

TEST_CASE("the primary buffer's format needs the priority level") {
    Rig rig;
    SoundBuffer *main = rig.Primary(0);
    WaveFormat format;
    format.channels = 2;
    format.bitsPerSample = 16;
    format.samplesPerSec = 44100;
    format.blockAlign = 4;
    format.avgBytesPerSec = 176400;
    CHECK(main->SetFormat(format) == kDsErrPrioLevelNeeded);
    REQUIRE(rig.device.SetCooperativeLevel(true, kDsSclPriority) == kDsOk);
    WaveFormat bad = format;
    bad.bitsPerSample = 12;
    CHECK(main->SetFormat(bad) == kDsErrBadFormat);
    CHECK(main->SetFormat(format) == kDsOk);
    CHECK(main->GetFormat().bitsPerSample == 16u);
    CHECK(rig.audio.log.Calls().empty());
}

TEST_CASE("the primary buffer's volume is the master gain and its pan is only kept") {
    Rig rig;
    SoundBuffer *main = rig.Primary(kDsbCapsCtrlVolume | kDsbCapsCtrlPan);
    CHECK(main->SetVolume(-2000) == kDsOk);
    CHECK(main->SetPan(500) == kDsOk);
    CHECK(rig.audio.log.Calls() == Calls{"SetMasterGain(0.1)"});
    std::int32_t pan = 0;
    CHECK(main->GetPan(&pan) == kDsOk);
    CHECK(pan == 500);
    std::uint32_t hz = 0;
    CHECK(main->GetFrequency(&hz) == kDsErrControlUnavail);
}

TEST_CASE("the primary buffer plays looping, has no bytes to lock and no position") {
    Rig rig;
    SoundBuffer *main = rig.Primary(0);
    std::uint32_t status = 9;
    CHECK(main->GetStatus(&status) == kDsOk);
    CHECK(status == 0u);
    CHECK(main->Play(0) == kDsErrInvalidParam);
    CHECK(main->Play(kDsbPlayLooping) == kDsOk);
    CHECK(main->GetStatus(&status) == kDsOk);
    CHECK(status == (kDsbStatusPlaying | kDsbStatusLooping));
    CHECK(main->Stop() == kDsOk);
    CHECK(main->GetStatus(&status) == kDsOk);
    CHECK(status == 0u);
    LockedRange range;
    CHECK(main->Lock(0, 4, 0, &range) == kDsErrPrioLevelNeeded);
    CHECK(main->Unlock(range) == kDsErrPrioLevelNeeded);
    CHECK(main->SetCurrentPosition(0) == kDsErrInvalidCall);
    std::uint32_t play = 1;
    std::uint32_t write = 1;
    CHECK(main->GetCurrentPosition(&play, &write) == kDsOk);
    CHECK(play == 0u);
    CHECK(write == 0u);
    CHECK(main->GetCaps().bufferBytes == 17640u);
    CHECK(main->Get3D() == nullptr);
    // Only GetStatus's draining of the completion queue reaches the output.
    CHECK(rig.audio.log.Calls().size() == rig.audio.log.Count("PollCompletion"));
}

} // namespace sfcop::hle::ds3d
