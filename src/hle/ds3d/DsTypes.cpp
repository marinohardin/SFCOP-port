#include "hle/ds3d/DsTypes.h"

#include <algorithm>
#include <cmath>

namespace sfcop::hle::ds3d {
namespace {

platform::Vector3 toVector3(const Vector &v) { return platform::Vector3{v.x, v.y, v.z}; }
bool finite(const Vector &v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
bool zero(const Vector &v) { return v.x == 0.0f && v.y == 0.0f && v.z == 0.0f; }

} // namespace

float gainFromVolume(std::int32_t volume) {
    if (volume <= kDsbVolumeMin) {
        return 0.0f;
    }
    return std::pow(10.0f, static_cast<float>((std::min)(volume, 0)) / 2000.0f);
}

float panFromDs(std::int32_t pan) {
    // The Audio component's 2D pan is DirectSound's own law with the unit changed: +-1 is
    // +-10000 hundredths of a decibel on the far side, the near side at unity.
    pan = std::clamp(pan, kDsbPanLeft, kDsbPanRight);
    return static_cast<float>(pan) / 10000.0f;
}

platform::Voice3D toVoice3D(const Buffer3DParams &p) {
    platform::Voice3D v;
    switch (p.mode) {
    case kDs3dModeHeadRelative:
        v.space = platform::VoiceSpace::kHeadRelative;
        break;
    case kDs3dModeDisable:
        v.space = platform::VoiceSpace::k2d;
        break;
    default:
        v.space = platform::VoiceSpace::kWorld;
        break;
    }
    v.position = toVector3(p.position);
    v.velocity = toVector3(p.velocity);
    v.minDistance = p.minDistance;
    // DirectSound accepts a maximum below the minimum; nothing attenuates past the minimum then.
    v.maxDistance = (std::max)(p.maxDistance, p.minDistance);
    v.coneInsideDegrees = p.insideConeAngle;
    v.coneOutsideDegrees = p.outsideConeAngle;
    v.coneOrientation = toVector3(p.coneOrientation);
    v.coneOutsideGain = gainFromVolume(p.coneOutsideVolume);
    return v;
}

platform::Listener toListener(const ListenerParams &p) {
    platform::Listener l;
    l.position = toVector3(p.position);
    l.velocity = toVector3(p.velocity);
    l.forward = toVector3(p.front);
    l.up = toVector3(p.top);
    l.distanceFactor = p.distanceFactor;
    l.rolloffFactor = p.rolloffFactor;
    l.dopplerFactor = p.dopplerFactor;
    return l;
}

bool validBuffer3D(const Buffer3DParams &p) {
    return finite(p.position) && finite(p.velocity) && finite(p.coneOrientation) &&
           p.insideConeAngle <= 360 && p.outsideConeAngle <= 360 &&
           p.insideConeAngle <= p.outsideConeAngle && p.coneOutsideVolume >= kDsbVolumeMin &&
           p.coneOutsideVolume <= 0 && p.minDistance > 0.0f && std::isfinite(p.minDistance) &&
           p.maxDistance > 0.0f && !std::isnan(p.maxDistance) && p.mode <= kDs3dModeDisable;
}

bool validListener(const ListenerParams &p) {
    return finite(p.position) && finite(p.velocity) && finite(p.front) && finite(p.top) &&
           !zero(p.front) && !zero(p.top) && p.distanceFactor > 0.0f &&
           std::isfinite(p.distanceFactor) && p.rolloffFactor >= 0.0f &&
           p.rolloffFactor <= 10.0f && p.dopplerFactor >= 0.0f && p.dopplerFactor <= 10.0f;
}

} // namespace sfcop::hle::ds3d
