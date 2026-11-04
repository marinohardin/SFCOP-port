// IDirectSound3DBuffer: the 3D parameters of one sound buffer, on Audio::SetVoice3D. A Set with
// DS3D_IMMEDIATE applies at once; with DS3D_DEFERRED it is stored and applied by the listener's
// CommitDeferredSettings. Get returns the latest values set either way. Mode DS3DMODE_NORMAL places
// the sound in the world, DS3DMODE_HEADRELATIVE relative to the listener, DS3DMODE_DISABLE turns
// 3D off (the buffer's own pan applies again).
#pragma once

#include "hle/ds3d/DsTypes.h"

namespace sfcop::hle::ds3d {

class Buffer3D {
public:
    // Applies the defaults to `voice` at once: a 3D buffer is positioned from its creation.
    Buffer3D(platform::Audio &audio, platform::VoiceId voice);

    // GetAllParameters and every single Get read from here.
    const Buffer3DParams &Parameters() const { return params_; }

    // Each returns kDsErrInvalidParam for a value out of range (angles over 360 or inside over
    // outside, a volume outside -10000 to 0, a distance not above 0, an unknown mode or apply
    // flag) and changes nothing then.
    HResult SetAllParameters(const Buffer3DParams &params, std::uint32_t apply);
    HResult SetConeAngles(std::uint32_t inside, std::uint32_t outside, std::uint32_t apply);
    HResult SetConeOrientation(const Vector &orientation, std::uint32_t apply);
    HResult SetConeOutsideVolume(std::int32_t volume, std::uint32_t apply);
    HResult SetMaxDistance(float distance, std::uint32_t apply);
    HResult SetMinDistance(float distance, std::uint32_t apply);
    HResult SetMode(std::uint32_t mode, std::uint32_t apply);
    HResult SetPosition(const Vector &position, std::uint32_t apply);
    HResult SetVelocity(const Vector &velocity, std::uint32_t apply);

    // Applies a deferred change, if any (CommitDeferredSettings).
    void ApplyPending();

private:
    HResult set(const Buffer3DParams &next, std::uint32_t apply);
    void apply();

    platform::Audio &audio_;
    platform::VoiceId voice_;
    Buffer3DParams params_;
    bool pending_ = false;
};

} // namespace sfcop::hle::ds3d
