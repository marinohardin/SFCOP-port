#include "hle/ds3d/Listener.h"

#include "hle/ds3d/DirectSound.h"

namespace sfcop::hle::ds3d {

Listener::Listener(platform::Audio &audio, DirectSound &device)
    : audio_(audio), device_(device) {}

void Listener::apply() {
    (void)audio_.SetListener(toListener(params_));
    pending_ = false;
}

HResult Listener::set(const ListenerParams &next, std::uint32_t apply) {
    if (apply > kDs3dDeferred || !validListener(next)) {
        return kDsErrInvalidParam;
    }
    params_ = next;
    if (apply == kDs3dDeferred) {
        pending_ = true;
    } else {
        this->apply();
    }
    return kDsOk;
}

void Listener::ApplyPending() {
    if (pending_) {
        apply();
    }
}

HResult Listener::SetAllParameters(const ListenerParams &params, std::uint32_t apply) {
    return set(params, apply);
}

HResult Listener::SetDistanceFactor(float factor, std::uint32_t apply) {
    ListenerParams next = params_;
    next.distanceFactor = factor;
    return set(next, apply);
}

HResult Listener::SetDopplerFactor(float factor, std::uint32_t apply) {
    ListenerParams next = params_;
    next.dopplerFactor = factor;
    return set(next, apply);
}

HResult Listener::SetOrientation(const Vector &front, const Vector &top, std::uint32_t apply) {
    ListenerParams next = params_;
    next.front = front;
    next.top = top;
    return set(next, apply);
}

HResult Listener::SetPosition(const Vector &position, std::uint32_t apply) {
    ListenerParams next = params_;
    next.position = position;
    return set(next, apply);
}

HResult Listener::SetRolloffFactor(float factor, std::uint32_t apply) {
    ListenerParams next = params_;
    next.rolloffFactor = factor;
    return set(next, apply);
}

HResult Listener::SetVelocity(const Vector &velocity, std::uint32_t apply) {
    ListenerParams next = params_;
    next.velocity = velocity;
    return set(next, apply);
}

HResult Listener::CommitDeferredSettings() {
    device_.CommitDeferred();
    return kDsOk;
}

} // namespace sfcop::hle::ds3d
