#include "hle/ds3d/Listener.h"

#include "hle/ds3d/Buffer3D.h"
#include "hle/ds3d/DirectSound.h"
#include "hle/ds3d/SoundBuffer.h"

#include "platform/Audio/AudioMock.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace sfcop::hle::ds3d {
namespace {

using platform::AudioMock;
using Calls = std::vector<std::string>;

struct Rig {
    AudioMock audio;
    DirectSound device{audio, hostMemory()};
    Listener *listener = nullptr;

    Rig() {
        REQUIRE(device.Open() == kDsOk);
        BufferDesc desc;
        desc.flags = kDsbCapsPrimaryBuffer | kDsbCapsCtrl3D;
        SoundBuffer *main = nullptr;
        REQUIRE(device.CreateSoundBuffer(desc, &main) == kDsOk);
        listener = device.GetListener();
        REQUIRE(listener != nullptr);
        audio.log.Clear();
    }
};

} // namespace

TEST_CASE("a new listener has DirectSound3D's defaults") {
    Rig rig;
    const ListenerParams &p = rig.listener->Parameters();
    CHECK(p.front.z == 1.0f);
    CHECK(p.top.y == 1.0f);
    CHECK(p.distanceFactor == 1.0f);
    CHECK(p.rolloffFactor == 1.0f);
    CHECK(p.dopplerFactor == 1.0f);
}

TEST_CASE("the listener's position, orientation and velocity reach the output") {
    Rig rig;
    CHECK(rig.listener->SetPosition(Vector{1.0f, 2.0f, 3.0f}, kDs3dImmediate) == kDsOk);
    CHECK(rig.audio.lastListener.position.y == 2.0f);
    CHECK(rig.listener->SetOrientation(Vector{1.0f, 0.0f, 0.0f}, Vector{0.0f, 1.0f, 0.0f},
                                       kDs3dImmediate) == kDsOk);
    CHECK(rig.audio.lastListener.forward.x == 1.0f);
    CHECK(rig.audio.lastListener.up.y == 1.0f);
    CHECK(rig.listener->SetVelocity(Vector{0.0f, 0.0f, 7.0f}, kDs3dImmediate) == kDsOk);
    CHECK(rig.audio.lastListener.velocity.z == 7.0f);
    CHECK(rig.audio.log.Calls() == Calls{"SetListener()", "SetListener()", "SetListener()"});
    CHECK(rig.listener->Parameters().front.x == 1.0f);
}

TEST_CASE("the distance, rolloff and Doppler factors reach the output") {
    Rig rig;
    CHECK(rig.listener->SetDistanceFactor(2.0f, kDs3dImmediate) == kDsOk);
    CHECK(rig.audio.lastListener.distanceFactor == 2.0f);
    CHECK(rig.listener->SetRolloffFactor(0.5f, kDs3dImmediate) == kDsOk);
    CHECK(rig.audio.lastListener.rolloffFactor == 0.5f);
    CHECK(rig.listener->SetDopplerFactor(3.0f, kDs3dImmediate) == kDsOk);
    CHECK(rig.audio.lastListener.dopplerFactor == 3.0f);
    CHECK(rig.listener->Parameters().distanceFactor == 2.0f);
}

TEST_CASE("SetAllParameters sets the whole listener at once") {
    Rig rig;
    ListenerParams p;
    p.position = Vector{9.0f, 0.0f, 0.0f};
    p.rolloffFactor = 2.0f;
    CHECK(rig.listener->SetAllParameters(p, kDs3dImmediate) == kDsOk);
    CHECK(rig.audio.log.Calls() == Calls{"SetListener()"});
    CHECK(rig.audio.lastListener.position.x == 9.0f);
    CHECK(rig.audio.lastListener.rolloffFactor == 2.0f);
}

TEST_CASE("an out-of-range listener parameter is refused and changes nothing") {
    Rig rig;
    CHECK(rig.listener->SetDistanceFactor(0.0f, kDs3dImmediate) == kDsErrInvalidParam);
    CHECK(rig.listener->SetRolloffFactor(10.5f, kDs3dImmediate) == kDsErrInvalidParam);
    CHECK(rig.listener->SetRolloffFactor(-0.5f, kDs3dImmediate) == kDsErrInvalidParam);
    CHECK(rig.listener->SetDopplerFactor(11.0f, kDs3dImmediate) == kDsErrInvalidParam);
    CHECK(rig.listener->SetOrientation(Vector{}, Vector{0.0f, 1.0f, 0.0f}, kDs3dImmediate) ==
          kDsErrInvalidParam);
    CHECK(rig.listener->SetOrientation(Vector{0.0f, 0.0f, 1.0f}, Vector{}, kDs3dImmediate) ==
          kDsErrInvalidParam);
    CHECK(rig.listener->SetPosition(Vector{}, 7) == kDsErrInvalidParam);
    CHECK(rig.listener->Parameters().distanceFactor == 1.0f);
    CHECK(rig.listener->Parameters().front.z == 1.0f);
    CHECK(rig.audio.log.Calls().empty());
}

TEST_CASE("CommitDeferredSettings applies the listener's and every 3D buffer's changes") {
    Rig rig;
    BufferDesc desc;
    desc.flags = kDsbCapsCtrl3D;
    desc.bufferBytes = 100;
    desc.hasFormat = true;
    desc.format.channels = 1;
    desc.format.bitsPerSample = 16;
    desc.format.samplesPerSec = 22050;
    desc.format.blockAlign = 2;
    SoundBuffer *buffer = nullptr;
    REQUIRE(rig.device.CreateSoundBuffer(desc, &buffer) == kDsOk);
    rig.audio.log.Clear();

    CHECK(rig.listener->SetPosition(Vector{0.0f, 0.0f, -4.0f}, kDs3dDeferred) == kDsOk);
    CHECK(rig.listener->SetDistanceFactor(2.0f, kDs3dDeferred) == kDsOk);
    CHECK(buffer->Get3D()->SetPosition(Vector{0.0f, 0.0f, 8.0f}, kDs3dDeferred) == kDsOk);
    CHECK(rig.audio.log.Calls().empty());
    CHECK(rig.listener->CommitDeferredSettings() == kDsOk);
    CHECK(rig.audio.log.Calls() == Calls{"SetListener()", "SetVoice3D(1, 1)"});
    CHECK(rig.audio.lastListener.position.z == -4.0f);
    CHECK(rig.audio.lastListener.distanceFactor == 2.0f);
    CHECK(rig.audio.lastVoice3D.position.z == 8.0f);
    CHECK(rig.listener->CommitDeferredSettings() == kDsOk);
    CHECK(rig.audio.log.Calls().size() == 2u);
}

} // namespace sfcop::hle::ds3d
