// The 3D part of the miniaudio backend: miniaudio's spatializer, which serves DirectSound3D's
// distance attenuation (miniaudio's inverse distance model, min / (min + rolloff (d - min)),
// clamped to the minimum and maximum distance; it equals DirectSound3D's (min / d) ^ rolloff only
// at a rolloff factor of 1, and differs from it at any other), pan from the direction relative to the listener, sound cones and
// the Doppler pitch. Internal to the backend; the miniaudio types stay in AudioSpatial.cpp.
// Not thread-safe: the backend calls it under its own lock.
#pragma once

#include "platform/Audio/Audio.h"

#include <cstddef>
#include <memory>

namespace sfcop::platform {

class SpatialScene {
public:
    // The listener for an output of `channels` (1 or 2) at `sampleRate` Hz.
    SpatialScene(unsigned channels, std::uint32_t sampleRate);
    ~SpatialScene();
    SpatialScene(const SpatialScene &) = delete;
    SpatialScene &operator=(const SpatialScene &) = delete;

    void SetListener(const Listener &listener);

    // One positioned sound. Created by NewSource(); valid while the scene lives.
    class Source {
    public:
        virtual ~Source() = default;
    };
    std::unique_ptr<Source> NewSource();

    // A voice's 3D parameters, with the listener's rolloff and Doppler factors.
    void SetSource(Source &source, const Voice3D &params);

    // Spatializes `frames` mono frames into `out` (interleaved, the scene's channels).
    void Process(Source &source, const float *mono, float *out, std::size_t frames);

    // The pitch ratio the source's motion gives, as of its last Process(); 1 without motion.
    float DopplerPitch(const Source &source) const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace sfcop::platform
