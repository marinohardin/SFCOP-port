// IDirectSound3DListener: the one listener of a DirectSound device, on Audio::SetListener.
// Immediate and deferred settings as Buffer3D.h describes; CommitDeferredSettings applies the
// listener's and every 3D buffer's deferred changes.
#pragma once

#include "hle/ds3d/DsTypes.h"

namespace sfcop::hle::ds3d {

class DirectSound;

class Listener {
public:
    Listener(platform::Audio &audio, DirectSound &device);

    const ListenerParams &Parameters() const { return params_; }

    // Each returns kDsErrInvalidParam for a value out of range (a distance factor not above 0,
    // a rolloff or Doppler factor outside 0 to 10, a zero front or top vector, an unknown apply
    // flag) and changes nothing then.
    HResult SetAllParameters(const ListenerParams &params, std::uint32_t apply);
    HResult SetDistanceFactor(float factor, std::uint32_t apply);
    HResult SetDopplerFactor(float factor, std::uint32_t apply);
    HResult SetOrientation(const Vector &front, const Vector &top, std::uint32_t apply);
    HResult SetPosition(const Vector &position, std::uint32_t apply);
    HResult SetRolloffFactor(float factor, std::uint32_t apply);
    HResult SetVelocity(const Vector &velocity, std::uint32_t apply);
    HResult CommitDeferredSettings();

    // Applies a deferred change, if any.
    void ApplyPending();

private:
    HResult set(const ListenerParams &next, std::uint32_t apply);
    void apply();

    platform::Audio &audio_;
    DirectSound &device_;
    ListenerParams params_;
    bool pending_ = false;
};

} // namespace sfcop::hle::ds3d
