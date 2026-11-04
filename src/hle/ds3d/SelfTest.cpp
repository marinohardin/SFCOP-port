// The spike's DirectSound3D self-test, check for check, against this module on the real mixer
// with a manual clock. As in src/hle/miles/SelfTest.cpp, each result the spike asserted is
// asserted on the module, and each call's stack purge on the shim source (Ds3dGuest.cpp). The
// spike's 3D objects stored their values and changed nothing heard; here they reach the mixer,
// so its one release check also listens for the buffer falling silent. 13 result checks and 13
// purge checks: 26 (README.md, "Self-test").
#include "hle/ds3d/Buffer3D.h"
#include "hle/ds3d/DirectSound.h"
#include "hle/ds3d/Listener.h"
#include "hle/ds3d/SoundBuffer.h"
#include "hle/miles/ShimRows.h"

#include "platform/Audio/AudioMiniaudio.h"

#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <vector>

namespace sfcop::hle::ds3d {
namespace {

using platform::AudioMiniaudio;
using platform::AudioOutput;

struct SelfTest {
    shimrows::Shims shims = shimrows::parseGuest(
        shimrows::readFile(shimrows::directoryOf(__FILE__) + "/Ds3dGuest.cpp"));
    int checks = 0;

    void ok(const char *what, bool cond) {
        ++checks;
        CAPTURE(what);
        CHECK(cond);
    }
    // The purge check of one call: the method's shim pops `argc` arguments, `this` included.
    void call(const std::string &method, int argc) {
        ++checks;
        CAPTURE(method);
        REQUIRE(shims.functionOf.count(method) == 1u);
        CHECK(shims.purgesOf[shims.functionOf[method]] == std::set<int>{argc});
    }
};

BufferDesc mono3D() {
    BufferDesc desc;
    desc.flags = kDsbCapsCtrl3D;
    desc.bufferBytes = 4410;
    desc.hasFormat = true;
    desc.format.channels = 1;
    desc.format.bitsPerSample = 16;
    desc.format.samplesPerSec = 44100;
    desc.format.blockAlign = 2;
    desc.format.avgBytesPerSec = 88200;
    return desc;
}

// The loudest sample of `frames` frames of output.
float loudest(AudioMiniaudio &audio, std::size_t frames) {
    std::vector<float> out(frames * 2);
    REQUIRE(audio.Render(out.data(), frames).Ok());
    float peak = 0.0f;
    for (float v : out) {
        peak = v > peak ? v : (-v > peak ? -v : peak);
    }
    return peak;
}

} // namespace

TEST_CASE("the spike's DirectSound3D self-test passes against the module: 26 checks") {
    SelfTest t;
    AudioMiniaudio audio(AudioOutput::kManualClock);
    DirectSound device(audio, hostMemory());
    REQUIRE(device.Open() == kDsOk);
    SoundBuffer *buffer = nullptr;
    REQUIRE(device.CreateSoundBuffer(mono3D(), &buffer) == kDsOk);
    LockedRange range;
    REQUIRE(buffer->Lock(0, 0, kDsbLockEntireBuffer, &range) == kDsOk);
    const std::int16_t half = 16384;
    for (std::uint32_t i = 0; i < range.firstBytes; i += 2) {
        std::memcpy(range.first + i, &half, 2);
    }
    REQUIRE(buffer->Unlock(range) == kDsOk);

    t.call("IDirectSoundBuffer::QueryInterface", 3);
    Buffer3D *b3 = buffer->Get3D();
    t.ok("QueryInterface(3D buffer)", b3 != nullptr);
    REQUIRE(b3 != nullptr);
    t.call("IDirectSound3DBuffer::SetPosition", 5);
    t.ok("SetPosition(1, 2, 3)",
         b3->SetPosition(Vector{1.0f, 2.0f, 3.0f}, kDs3dImmediate) == kDsOk);
    t.call("IDirectSound3DBuffer::GetPosition", 2);
    const Vector &at = b3->Parameters().position;
    t.ok("GetPosition", at.x == 1.0f && at.y == 2.0f && at.z == 3.0f);
    t.call("IDirectSound3DBuffer::SetConeOutsideVolume", 3);
    t.ok("SetConeOutsideVolume(-600)", b3->SetConeOutsideVolume(-600, kDs3dImmediate) == kDsOk);
    t.call("IDirectSound3DBuffer::GetConeOutsideVolume", 2);
    t.ok("GetConeOutsideVolume", b3->Parameters().coneOutsideVolume == -600);
    t.call("IDirectSound3DBuffer::GetMaxDistance", 2);
    t.ok("default max distance 1e9", b3->Parameters().maxDistance == 1000000000.0f);

    REQUIRE(buffer->Play(kDsbPlayLooping) == kDsOk);
    REQUIRE(loudest(audio, 1024) > 0.0f);
    t.call("IDirectSound3DBuffer::Release", 1);
    device.ReleaseBuffer(buffer);
    t.ok("Release the 3D buffer", loudest(audio, 1024) == 0.0f);

    BufferDesc primary;
    primary.flags = kDsbCapsPrimaryBuffer | kDsbCapsCtrl3D;
    SoundBuffer *main = nullptr;
    REQUIRE(device.CreateSoundBuffer(primary, &main) == kDsOk);
    t.call("IDirectSoundBuffer::QueryInterface", 3);
    Listener *l = device.GetListener();
    t.ok("QueryInterface(listener)", l != nullptr);
    REQUIRE(l != nullptr);
    t.call("IDirectSound3DListener::SetOrientation", 8);
    t.ok("SetOrientation", l->SetOrientation(Vector{0.0f, 0.0f, 1.0f}, Vector{0.0f, 1.0f, 0.0f},
                                             kDs3dImmediate) == kDsOk);
    t.call("IDirectSound3DListener::GetOrientation", 3);
    t.ok("GetOrientation", l->Parameters().front.z == 1.0f && l->Parameters().top.y == 1.0f);
    t.call("IDirectSound3DListener::SetDistanceFactor", 3);
    t.ok("SetDistanceFactor(2)", l->SetDistanceFactor(2.0f, kDs3dImmediate) == kDsOk);
    t.call("IDirectSound3DListener::GetDistanceFactor", 2);
    t.ok("GetDistanceFactor", l->Parameters().distanceFactor == 2.0f);
    t.call("IDirectSound3DListener::CommitDeferredSettings", 1);
    t.ok("CommitDeferredSettings", l->CommitDeferredSettings() == kDsOk);

    CHECK(t.checks == 26);
}

} // namespace sfcop::hle::ds3d
