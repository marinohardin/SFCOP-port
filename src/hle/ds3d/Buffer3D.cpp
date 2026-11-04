#include "hle/ds3d/Buffer3D.h"

namespace sfcop::hle::ds3d {

Buffer3D::Buffer3D(platform::Audio &audio, platform::VoiceId voice)
    : audio_(audio), voice_(voice) {
    apply();
}

void Buffer3D::apply() {
    (void)audio_.SetVoice3D(voice_, toVoice3D(params_));
    pending_ = false;
}

HResult Buffer3D::set(const Buffer3DParams &next, std::uint32_t apply) {
    if (apply > kDs3dDeferred || !validBuffer3D(next)) {
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

void Buffer3D::ApplyPending() {
    if (pending_) {
        apply();
    }
}

HResult Buffer3D::SetAllParameters(const Buffer3DParams &params, std::uint32_t apply) {
    return set(params, apply);
}

HResult Buffer3D::SetConeAngles(std::uint32_t inside, std::uint32_t outside,
                                std::uint32_t apply) {
    Buffer3DParams next = params_;
    next.insideConeAngle = inside;
    next.outsideConeAngle = outside;
    return set(next, apply);
}

HResult Buffer3D::SetConeOrientation(const Vector &orientation, std::uint32_t apply) {
    Buffer3DParams next = params_;
    next.coneOrientation = orientation;
    return set(next, apply);
}

HResult Buffer3D::SetConeOutsideVolume(std::int32_t volume, std::uint32_t apply) {
    Buffer3DParams next = params_;
    next.coneOutsideVolume = volume;
    return set(next, apply);
}

HResult Buffer3D::SetMaxDistance(float distance, std::uint32_t apply) {
    Buffer3DParams next = params_;
    next.maxDistance = distance;
    return set(next, apply);
}

HResult Buffer3D::SetMinDistance(float distance, std::uint32_t apply) {
    Buffer3DParams next = params_;
    next.minDistance = distance;
    return set(next, apply);
}

HResult Buffer3D::SetMode(std::uint32_t mode, std::uint32_t apply) {
    Buffer3DParams next = params_;
    next.mode = mode;
    return set(next, apply);
}

HResult Buffer3D::SetPosition(const Vector &position, std::uint32_t apply) {
    Buffer3DParams next = params_;
    next.position = position;
    return set(next, apply);
}

HResult Buffer3D::SetVelocity(const Vector &velocity, std::uint32_t apply) {
    Buffer3DParams next = params_;
    next.velocity = velocity;
    return set(next, apply);
}

} // namespace sfcop::hle::ds3d
