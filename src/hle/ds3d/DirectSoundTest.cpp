#include "hle/ds3d/DirectSound.h"

#include "hle/ds3d/Buffer3D.h"
#include "hle/ds3d/Listener.h"
#include "hle/ds3d/SoundBuffer.h"

#include "platform/Audio/AudioMock.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace sfcop::hle::ds3d {
namespace {

using platform::AudioMock;
using platform::Error;
using platform::VoiceId;
using Calls = std::vector<std::string>;

BufferDesc secondary(std::uint16_t channels, std::uint16_t bits, std::uint32_t rate,
                     std::uint32_t bytes, std::uint32_t flags = 0) {
    BufferDesc desc;
    desc.flags = flags;
    desc.bufferBytes = bytes;
    desc.hasFormat = true;
    desc.format.channels = channels;
    desc.format.bitsPerSample = bits;
    desc.format.samplesPerSec = rate;
    desc.format.blockAlign = static_cast<std::uint16_t>(channels * bits / 8);
    desc.format.avgBytesPerSec = rate * desc.format.blockAlign;
    return desc;
}

BufferDesc primary(std::uint32_t flags = 0) {
    BufferDesc desc;
    desc.flags = kDsbCapsPrimaryBuffer | flags;
    return desc;
}

struct Rig {
    AudioMock audio;
    DirectSound device{audio, hostMemory()};

    Rig() {
        REQUIRE(device.Open() == kDsOk);
        audio.log.Clear();
    }
};

} // namespace

TEST_CASE("DirectSoundCreate opens 44.1 kHz stereo once") {
    AudioMock audio;
    DirectSound device{audio, hostMemory()};
    CHECK(device.Open() == kDsOk);
    CHECK(device.Open() == kDsOk);
    CHECK(audio.log.Calls() == Calls{"Open(44100, 2)"});
}

TEST_CASE("DirectSoundCreate reports an output that does not open as no driver") {
    AudioMock audio;
    audio.failWith = Error::kIoError;
    DirectSound device{audio, hostMemory()};
    CHECK(device.Open() == kDsErrNoDriver);
    SoundBuffer *buffer = nullptr;
    CHECK(device.CreateSoundBuffer(primary(), &buffer) == kDsErrInvalidCall);
    CHECK(buffer == nullptr);
}

TEST_CASE("DirectSoundCreate uses an output that is open already") {
    AudioMock audio;
    audio.failWith = Error::kWrongState;
    DirectSound device{audio, hostMemory()};
    CHECK(device.Open() == kDsOk);
}

TEST_CASE("the device closes its output after destroying its buffers") {
    AudioMock audio;
    {
        DirectSound device{audio, hostMemory()};
        REQUIRE(device.Open() == kDsOk);
        SoundBuffer *buffer = nullptr;
        REQUIRE(device.CreateSoundBuffer(secondary(1, 16, 22050, 100), &buffer) == kDsOk);
        audio.log.Clear();
    }
    CHECK(audio.log.Calls() == Calls{"DestroyVoice(1)", "ReleaseClip(1)", "Close()"});
}

TEST_CASE("SetCooperativeLevel needs a window and a known level") {
    Rig rig;
    CHECK(rig.device.CooperativeLevel() == 0);
    CHECK(rig.device.SetCooperativeLevel(false, kDsSclPriority) == kDsErrInvalidParam);
    CHECK(rig.device.SetCooperativeLevel(true, 0) == kDsErrInvalidParam);
    CHECK(rig.device.SetCooperativeLevel(true, 5) == kDsErrInvalidParam);
    CHECK(rig.device.CooperativeLevel() == 0);
    CHECK(rig.device.SetCooperativeLevel(true, kDsSclPriority) == kDsOk);
    CHECK(rig.device.CooperativeLevel() == kDsSclPriority);
    CHECK(rig.audio.log.Calls().empty());
}

TEST_CASE("GetCaps describes a device mixing 64 buffers at 100 to 100000 Hz") {
    Rig rig;
    Caps caps = rig.device.GetCaps();
    CHECK(caps.flags == 0x0F5Fu);
    CHECK(caps.minSecondarySampleRate == 100u);
    CHECK(caps.maxSecondarySampleRate == 100000u);
    CHECK(caps.primaryBuffers == 1u);
    CHECK(caps.maxHwMixingAllBuffers == 64u);
    CHECK(caps.maxHwMixingStaticBuffers == 64u);
    CHECK(caps.maxHwMixingStreamingBuffers == 64u);
    CHECK(caps.freeHwMixingAllBuffers == 64u);
    CHECK(caps.freeHwMixingStaticBuffers == 64u);
    CHECK(caps.freeHwMixingStreamingBuffers == 64u);
}

TEST_CASE("the primary buffer is made once and handed out again") {
    Rig rig;
    SoundBuffer *first = nullptr;
    SoundBuffer *second = nullptr;
    CHECK(rig.device.CreateSoundBuffer(primary(kDsbCapsCtrlVolume), &first) == kDsOk);
    CHECK(rig.device.CreateSoundBuffer(primary(kDsbCapsCtrlVolume), &second) == kDsOk);
    REQUIRE(first != nullptr);
    CHECK(first == second);
    CHECK(first->IsPrimary());
    CHECK(rig.device.GetListener() == nullptr);
    CHECK(rig.audio.log.Calls().empty());
}

TEST_CASE("a primary buffer with DSBCAPS_CTRL3D carries the listener") {
    Rig rig;
    SoundBuffer *buffer = nullptr;
    CHECK(rig.device.CreateSoundBuffer(primary(kDsbCapsCtrl3D), &buffer) == kDsOk);
    Listener *listener = rig.device.GetListener();
    REQUIRE(listener != nullptr);
    rig.device.ReleaseBuffer(buffer);
    CHECK(rig.device.GetListener() == listener);
}

TEST_CASE("a primary buffer takes no size, no format and no frequency control") {
    Rig rig;
    SoundBuffer *buffer = nullptr;
    BufferDesc sized = primary();
    sized.bufferBytes = 4096;
    CHECK(rig.device.CreateSoundBuffer(sized, &buffer) == kDsErrInvalidParam);
    BufferDesc formatted = primary();
    formatted.hasFormat = true;
    CHECK(rig.device.CreateSoundBuffer(formatted, &buffer) == kDsErrInvalidParam);
    CHECK(rig.device.CreateSoundBuffer(primary(kDsbCapsCtrlFrequency), &buffer) ==
          kDsErrInvalidParam);
    CHECK(rig.device.CreateSoundBuffer(primary(kDsbCapsStatic), &buffer) == kDsErrInvalidParam);
    CHECK(buffer == nullptr);
    CHECK(rig.device.CreateSoundBuffer(primary(), nullptr) == kDsErrInvalidParam);
}

TEST_CASE("a secondary buffer is a silent clip and a stopped voice") {
    Rig rig;
    SoundBuffer *buffer = nullptr;
    CHECK(rig.device.CreateSoundBuffer(secondary(1, 8, 22050, 100), &buffer) == kDsOk);
    REQUIRE(buffer != nullptr);
    CHECK_FALSE(buffer->IsPrimary());
    CHECK(rig.audio.log.Calls() == Calls{"LoadClip(0, 1, 22050, 100)", "CreateVoice(1)"});
    CHECK(buffer->Get3D() == nullptr);
}

TEST_CASE("a secondary buffer's clip holds whole frames") {
    Rig rig;
    SoundBuffer *buffer = nullptr;
    CHECK(rig.device.CreateSoundBuffer(secondary(2, 16, 44100, 4002), &buffer) == kDsOk);
    CHECK(rig.audio.log.Calls() == Calls{"LoadClip(1, 2, 44100, 4000)", "CreateVoice(1)"});
    CHECK(buffer->GetCaps().bufferBytes == 4002u);
}

TEST_CASE("a buffer slower than 1000 Hz plays a 1000 Hz clip at its own rate") {
    Rig rig;
    SoundBuffer *buffer = nullptr;
    CHECK(rig.device.CreateSoundBuffer(secondary(1, 16, 500, 100), &buffer) == kDsOk);
    CHECK(rig.audio.log.Calls() ==
          Calls{"LoadClip(1, 1, 1000, 100)", "CreateVoice(1)", "SetRate(1, 500)"});
}

TEST_CASE("a secondary buffer needs a size and a PCM format it can play") {
    Rig rig;
    SoundBuffer *buffer = nullptr;
    BufferDesc noFormat = secondary(1, 16, 22050, 100);
    noFormat.hasFormat = false;
    CHECK(rig.device.CreateSoundBuffer(noFormat, &buffer) == kDsErrInvalidParam);
    CHECK(rig.device.CreateSoundBuffer(secondary(1, 16, 22050, 2), &buffer) ==
          kDsErrInvalidParam);
    CHECK(rig.device.CreateSoundBuffer(secondary(1, 16, 22050, 0x10000000), &buffer) ==
          kDsErrInvalidParam);
    CHECK(rig.device.CreateSoundBuffer(secondary(1, 24, 22050, 99), &buffer) == kDsErrBadFormat);
    CHECK(rig.device.CreateSoundBuffer(secondary(3, 16, 22050, 120), &buffer) ==
          kDsErrBadFormat);
    CHECK(rig.device.CreateSoundBuffer(secondary(1, 16, 99, 100), &buffer) == kDsErrBadFormat);
    CHECK(rig.device.CreateSoundBuffer(secondary(1, 16, 100001, 100), &buffer) ==
          kDsErrBadFormat);
    BufferDesc misaligned = secondary(2, 16, 22050, 100);
    misaligned.format.blockAlign = 2;
    CHECK(rig.device.CreateSoundBuffer(misaligned, &buffer) == kDsErrBadFormat);
    BufferDesc compressed = secondary(1, 16, 22050, 100);
    compressed.format.formatTag = 2;
    CHECK(rig.device.CreateSoundBuffer(compressed, &buffer) == kDsErrBadFormat);
    CHECK(buffer == nullptr);
    CHECK(rig.audio.log.Calls().empty());
}

TEST_CASE("a 3D buffer is mono and positioned from its creation") {
    Rig rig;
    SoundBuffer *buffer = nullptr;
    CHECK(rig.device.CreateSoundBuffer(secondary(2, 16, 22050, 100, kDsbCapsCtrl3D), &buffer) ==
          kDsErrInvalidParam);
    CHECK(rig.device.CreateSoundBuffer(secondary(1, 16, 22050, 100, kDsbCapsCtrl3D), &buffer) ==
          kDsOk);
    REQUIRE(buffer->Get3D() != nullptr);
    CHECK(rig.audio.log.Calls() ==
          Calls{"LoadClip(1, 1, 22050, 100)", "CreateVoice(1)", "SetVoice3D(1, 1)"});
}

TEST_CASE("a buffer whose clip or voice cannot be had is out of memory") {
    Rig rig;
    SoundBuffer *buffer = nullptr;
    rig.audio.loadClipResult = Error::kOutOfResources;
    CHECK(rig.device.CreateSoundBuffer(secondary(1, 16, 22050, 100), &buffer) ==
          kDsErrOutOfMemory);
    rig.audio.loadClipResult = platform::ClipId{1};
    rig.audio.createVoiceResult = Error::kOutOfResources;
    rig.audio.log.Clear();
    CHECK(rig.device.CreateSoundBuffer(secondary(1, 16, 22050, 100), &buffer) ==
          kDsErrOutOfMemory);
    CHECK(rig.audio.log.Calls() ==
          Calls{"LoadClip(1, 1, 22050, 100)", "CreateVoice(1)", "ReleaseClip(1)"});
    CHECK(buffer == nullptr);
}

TEST_CASE("a duplicate shares the clip and copies the volume, pan and frequency") {
    Rig rig;
    constexpr std::uint32_t kControls = kDsbCapsCtrlVolume | kDsbCapsCtrlPan |
                                        kDsbCapsCtrlFrequency;
    SoundBuffer *original = nullptr;
    REQUIRE(rig.device.CreateSoundBuffer(secondary(1, 16, 22050, 100, kControls), &original) ==
            kDsOk);
    REQUIRE(original->SetVolume(-2000) == kDsOk);
    REQUIRE(original->SetFrequency(11025) == kDsOk);
    rig.audio.createVoiceResult = VoiceId{2};
    rig.audio.log.Clear();
    SoundBuffer *copy = nullptr;
    CHECK(rig.device.DuplicateSoundBuffer(*original, &copy) == kDsOk);
    REQUIRE(copy != nullptr);
    CHECK(copy != original);
    CHECK(rig.audio.log.Calls() == Calls{"CreateVoice(1)", "SetGain(2, 0.1)", "SetRate(2, 11025)"});
    std::int32_t volume = 0;
    CHECK(copy->GetVolume(&volume) == kDsOk);
    CHECK(volume == -2000);
    CHECK(copy->Flags() == kControls);
    CHECK(copy->GetFormat().samplesPerSec == 22050u);

    // The clip goes with the last buffer that plays it.
    rig.audio.log.Clear();
    rig.device.ReleaseBuffer(original);
    CHECK(rig.audio.log.Calls() == Calls{"DestroyVoice(1)"});
    rig.device.ReleaseBuffer(copy);
    CHECK(rig.audio.log.Calls() == Calls{"DestroyVoice(1)", "DestroyVoice(2)", "ReleaseClip(1)"});
}

TEST_CASE("a duplicate of a 3D buffer gets its own 3D parameters") {
    Rig rig;
    SoundBuffer *original = nullptr;
    REQUIRE(rig.device.CreateSoundBuffer(secondary(1, 16, 22050, 100, kDsbCapsCtrl3D),
                                         &original) == kDsOk);
    REQUIRE(original->Get3D()->SetPosition(Vector{1.0f, 2.0f, 3.0f}, kDs3dImmediate) == kDsOk);
    SoundBuffer *copy = nullptr;
    REQUIRE(rig.device.DuplicateSoundBuffer(*original, &copy) == kDsOk);
    REQUIRE(copy->Get3D() != nullptr);
    CHECK(copy->Get3D()->Parameters().position.x == 0.0f);
}

TEST_CASE("the primary buffer cannot be duplicated") {
    Rig rig;
    SoundBuffer *main = nullptr;
    REQUIRE(rig.device.CreateSoundBuffer(primary(), &main) == kDsOk);
    SoundBuffer *copy = nullptr;
    CHECK(rig.device.DuplicateSoundBuffer(*main, &copy) == kDsErrInvalidCall);
    CHECK(copy == nullptr);
}

TEST_CASE("the speaker configuration reads stereo until set") {
    Rig rig;
    CHECK(rig.device.GetSpeakerConfig() == kDsSpeakerStereo);
    CHECK(rig.device.SetSpeakerConfig(2) == kDsOk);
    CHECK(rig.device.GetSpeakerConfig() == 2u);
}

TEST_CASE("Compact succeeds and Initialize finds the device initialized") {
    Rig rig;
    CHECK(rig.device.Compact() == kDsOk);
    CHECK(rig.device.Initialize() == kDsErrAlreadyInitialized);
}

TEST_CASE("DrainCompletions empties the completion queue") {
    Rig rig;
    rig.audio.completions.push_back(platform::Completion{});
    rig.audio.completions.push_back(platform::Completion{});
    rig.device.DrainCompletions();
    CHECK(rig.audio.completions.empty());
}

} // namespace sfcop::hle::ds3d
