#include "hle/ds3d/Buffer3D.h"

#include "hle/ds3d/DirectSound.h"
#include "hle/ds3d/Listener.h"
#include "hle/ds3d/SoundBuffer.h"

#include "platform/Audio/AudioMiniaudio.h"
#include "platform/Audio/AudioMock.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace sfcop::hle::ds3d {
namespace {

using platform::AudioMock;
using platform::VoiceSpace;
using Calls = std::vector<std::string>;

BufferDesc mono3D(std::uint32_t bytes = 200) {
    BufferDesc desc;
    desc.flags = kDsbCapsCtrl3D | kDsbCapsCtrlVolume;
    desc.bufferBytes = bytes;
    desc.hasFormat = true;
    desc.format.channels = 1;
    desc.format.bitsPerSample = 16;
    desc.format.samplesPerSec = 44100;
    desc.format.blockAlign = 2;
    desc.format.avgBytesPerSec = 88200;
    return desc;
}

struct Rig {
    AudioMock audio;
    DirectSound device{audio, hostMemory()};
    Buffer3D *b3d = nullptr;

    Rig() {
        REQUIRE(device.Open() == kDsOk);
        SoundBuffer *buffer = nullptr;
        REQUIRE(device.CreateSoundBuffer(mono3D(), &buffer) == kDsOk);
        b3d = buffer->Get3D();
        REQUIRE(b3d != nullptr);
        audio.log.Clear();
    }
};

} // namespace

TEST_CASE("a new 3D buffer has DirectSound3D's defaults, applied to its voice") {
    Rig rig;
    const Buffer3DParams &p = rig.b3d->Parameters();
    CHECK(p.insideConeAngle == 360u);
    CHECK(p.outsideConeAngle == 360u);
    CHECK(p.coneOrientation.z == 1.0f);
    CHECK(p.coneOutsideVolume == 0);
    CHECK(p.minDistance == 1.0f);
    CHECK(p.maxDistance == 1000000000.0f);
    CHECK(p.mode == kDs3dModeNormal);
    CHECK(rig.audio.lastVoice3D.space == VoiceSpace::kWorld);
    CHECK(rig.audio.lastVoice3D.minDistance == 1.0f);
    CHECK(rig.audio.lastVoice3D.maxDistance == 1000000000.0f);
    CHECK(rig.audio.lastVoice3D.coneOutsideGain == 1.0f);
}

TEST_CASE("an immediate SetPosition reaches the voice at once") {
    Rig rig;
    CHECK(rig.b3d->SetPosition(Vector{1.0f, 2.0f, 3.0f}, kDs3dImmediate) == kDsOk);
    CHECK(rig.audio.log.Calls() == Calls{"SetVoice3D(1, 1)"});
    CHECK(rig.audio.lastVoice3D.position.x == 1.0f);
    CHECK(rig.audio.lastVoice3D.position.y == 2.0f);
    CHECK(rig.audio.lastVoice3D.position.z == 3.0f);
    CHECK(rig.b3d->Parameters().position.z == 3.0f);
}

TEST_CASE("a deferred change reads back at once and reaches the voice on commit") {
    Rig rig;
    CHECK(rig.b3d->SetVelocity(Vector{0.0f, 0.0f, -5.0f}, kDs3dDeferred) == kDsOk);
    CHECK(rig.b3d->SetMinDistance(10.0f, kDs3dDeferred) == kDsOk);
    CHECK(rig.b3d->Parameters().velocity.z == -5.0f);
    CHECK(rig.b3d->Parameters().minDistance == 10.0f);
    CHECK(rig.audio.log.Calls().empty());
    rig.device.CommitDeferred();
    CHECK(rig.audio.log.Calls() == Calls{"SetVoice3D(1, 1)"});
    CHECK(rig.audio.lastVoice3D.velocity.z == -5.0f);
    CHECK(rig.audio.lastVoice3D.minDistance == 10.0f);
    rig.device.CommitDeferred();
    CHECK(rig.audio.log.Calls().size() == 1u);
}

TEST_CASE("the cone's outside volume is a gain in hundredths of a decibel") {
    Rig rig;
    CHECK(rig.b3d->SetConeOutsideVolume(-600, kDs3dImmediate) == kDsOk);
    CHECK(rig.b3d->Parameters().coneOutsideVolume == -600);
    // -6 dB: 10^(-600 / 2000) = 0.5012.
    CHECK(rig.audio.lastVoice3D.coneOutsideGain == doctest::Approx(0.5012f).epsilon(0.001));
    CHECK(rig.b3d->SetConeAngles(90, 180, kDs3dImmediate) == kDsOk);
    CHECK(rig.audio.lastVoice3D.coneInsideDegrees == 90u);
    CHECK(rig.audio.lastVoice3D.coneOutsideDegrees == 180u);
    CHECK(rig.b3d->SetConeOrientation(Vector{1.0f, 0.0f, 0.0f}, kDs3dImmediate) == kDsOk);
    CHECK(rig.audio.lastVoice3D.coneOrientation.x == 1.0f);
}

TEST_CASE("the mode places the voice in the world, relative to the listener, or in 2D") {
    Rig rig;
    CHECK(rig.b3d->SetMode(kDs3dModeHeadRelative, kDs3dImmediate) == kDsOk);
    CHECK(rig.audio.lastVoice3D.space == VoiceSpace::kHeadRelative);
    CHECK(rig.b3d->SetMode(kDs3dModeDisable, kDs3dImmediate) == kDsOk);
    CHECK(rig.audio.lastVoice3D.space == VoiceSpace::k2d);
    CHECK(rig.b3d->SetMode(kDs3dModeNormal, kDs3dImmediate) == kDsOk);
    CHECK(rig.audio.lastVoice3D.space == VoiceSpace::kWorld);
}

TEST_CASE("a maximum distance below the minimum reads back and attenuates nothing further") {
    Rig rig;
    CHECK(rig.b3d->SetMinDistance(50.0f, kDs3dImmediate) == kDsOk);
    CHECK(rig.b3d->SetMaxDistance(20.0f, kDs3dImmediate) == kDsOk);
    CHECK(rig.b3d->Parameters().maxDistance == 20.0f);
    CHECK(rig.audio.lastVoice3D.minDistance == 50.0f);
    CHECK(rig.audio.lastVoice3D.maxDistance == 50.0f);
}

TEST_CASE("SetAllParameters sets everything at once") {
    Rig rig;
    Buffer3DParams p;
    p.position = Vector{4.0f, 5.0f, 6.0f};
    p.insideConeAngle = 30;
    p.outsideConeAngle = 60;
    p.minDistance = 2.0f;
    p.maxDistance = 200.0f;
    p.mode = kDs3dModeHeadRelative;
    CHECK(rig.b3d->SetAllParameters(p, kDs3dImmediate) == kDsOk);
    CHECK(rig.audio.log.Calls() == Calls{"SetVoice3D(1, 2)"});
    CHECK(rig.audio.lastVoice3D.position.y == 5.0f);
    CHECK(rig.audio.lastVoice3D.coneInsideDegrees == 30u);
    CHECK(rig.audio.lastVoice3D.maxDistance == 200.0f);
    CHECK(rig.b3d->Parameters().outsideConeAngle == 60u);
}

TEST_CASE("an out-of-range 3D parameter is refused and changes nothing") {
    Rig rig;
    CHECK(rig.b3d->SetConeAngles(361, 361, kDs3dImmediate) == kDsErrInvalidParam);
    CHECK(rig.b3d->SetConeAngles(180, 90, kDs3dImmediate) == kDsErrInvalidParam);
    CHECK(rig.b3d->SetConeOutsideVolume(1, kDs3dImmediate) == kDsErrInvalidParam);
    CHECK(rig.b3d->SetConeOutsideVolume(-10001, kDs3dImmediate) == kDsErrInvalidParam);
    CHECK(rig.b3d->SetMinDistance(0.0f, kDs3dImmediate) == kDsErrInvalidParam);
    CHECK(rig.b3d->SetMaxDistance(-1.0f, kDs3dImmediate) == kDsErrInvalidParam);
    CHECK(rig.b3d->SetMode(3, kDs3dImmediate) == kDsErrInvalidParam);
    CHECK(rig.b3d->SetPosition(Vector{NAN, 0.0f, 0.0f}, kDs3dImmediate) == kDsErrInvalidParam);
    CHECK(rig.b3d->SetPosition(Vector{1.0f, 0.0f, 0.0f}, 2) == kDsErrInvalidParam);
    CHECK(rig.b3d->Parameters().position.x == 0.0f);
    CHECK(rig.b3d->Parameters().insideConeAngle == 360u);
    CHECK(rig.b3d->Parameters().minDistance == 1.0f);
    CHECK(rig.audio.log.Calls().empty());
}

namespace {

// Plays a 3D buffer of constant 0.5 at `position` (minimum distance 10) through the real mixer
// and returns the left and right levels of a frame after the gains have settled.
std::pair<float, float> heardAt(const Vector &position) {
    platform::AudioMiniaudio audio(platform::AudioOutput::kManualClock);
    DirectSound device{audio, hostMemory()};
    REQUIRE(device.Open() == kDsOk);
    SoundBuffer *buffer = nullptr;
    REQUIRE(device.CreateSoundBuffer(mono3D(8820), &buffer) == kDsOk);
    LockedRange range;
    REQUIRE(buffer->Lock(0, 0, kDsbLockEntireBuffer, &range) == kDsOk);
    const std::int16_t half = 16384;
    for (std::uint32_t i = 0; i < range.firstBytes; i += 2) {
        std::memcpy(range.first + i, &half, 2);
    }
    REQUIRE(buffer->Unlock(range) == kDsOk);
    REQUIRE(buffer->Get3D()->SetMinDistance(10.0f, kDs3dDeferred) == kDsOk);
    REQUIRE(buffer->Get3D()->SetPosition(position, kDs3dDeferred) == kDsOk);
    device.CommitDeferred();
    REQUIRE(buffer->Play(kDsbPlayLooping) == kDsOk);
    std::vector<float> out(2048 * 2);
    REQUIRE(audio.Render(out.data(), 2048).Ok());
    return {out[out.size() - 2], out[out.size() - 1]};
}

} // namespace

TEST_CASE("a DirectSound3D buffer is quieter further away and louder on its own side") {
    auto ahead20 = heardAt(Vector{0.0f, 0.0f, 20.0f});
    auto ahead40 = heardAt(Vector{0.0f, 0.0f, 40.0f});
    auto right20 = heardAt(Vector{20.0f, 0.0f, 0.0f});
    auto left20 = heardAt(Vector{-20.0f, 0.0f, 0.0f});
    // Minimum distance 10, rolloff 1: half the gain at 20, a quarter at 40. Ahead is centred
    // at 0.5 a channel, so 0.5 x 0.5 x 0.5 = 0.125 at 20.
    CHECK(ahead20.first == doctest::Approx(0.125f).epsilon(0.02));
    CHECK(ahead20.second == doctest::Approx(0.125f).epsilon(0.02));
    CHECK(ahead40.first == doctest::Approx(0.0625f).epsilon(0.02));
    CHECK(right20.second > right20.first);
    CHECK(left20.first > left20.second);
    CHECK(right20.second == doctest::Approx(left20.first));
}

} // namespace sfcop::hle::ds3d
