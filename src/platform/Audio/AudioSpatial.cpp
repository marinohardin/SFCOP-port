#include "platform/Audio/AudioSpatial.h"

#include <miniaudio.h>

#include <algorithm>

namespace sfcop::platform {
namespace {

constexpr float kSpeedOfSound = 343.3f;     // metres per second, as DirectSound3D and OpenAL
constexpr float kSmallestDistance = 1e-6f;  // a zero minimum distance would silence everything
constexpr float kDegrees = 3.14159265358979f / 180.0f;

class MaSource final : public SpatialScene::Source {
public:
    MaSource(unsigned channels, std::uint32_t sampleRate) {
        ma_spatializer_config config = ma_spatializer_config_init(1, channels);
        config.handedness = ma_handedness_left;
        config.attenuationModel = ma_attenuation_model_inverse;
        // About 5 ms of gain smoothing: no clicks when a source moves, and settled within a
        // mix period.
        config.gainSmoothTimeInFrames = sampleRate / 200;
        ok = ma_spatializer_init(&config, nullptr, &spatializer) == MA_SUCCESS;
    }
    ~MaSource() override {
        if (ok) {
            ma_spatializer_uninit(&spatializer, nullptr);
        }
    }
    MaSource(const MaSource &) = delete;
    MaSource &operator=(const MaSource &) = delete;

    ma_spatializer spatializer{};
    bool ok = false;
};

} // namespace

struct SpatialScene::State {
    ma_spatializer_listener listener{};
    bool ok = false;
    unsigned channels = 2;
    std::uint32_t sampleRate = 44100;
    float rolloff = 1.0f;
    float doppler = 1.0f;
};

SpatialScene::SpatialScene(unsigned channels, std::uint32_t sampleRate)
    : state_(std::make_unique<State>()) {
    state_->channels = channels;
    state_->sampleRate = sampleRate;
    ma_spatializer_listener_config config = ma_spatializer_listener_config_init(channels);
    config.handedness = ma_handedness_left;
    config.speedOfSound = kSpeedOfSound;
    state_->ok = ma_spatializer_listener_init(&config, nullptr, &state_->listener) == MA_SUCCESS;
    SetListener(Listener{});
}

SpatialScene::~SpatialScene() {
    if (state_->ok) {
        ma_spatializer_listener_uninit(&state_->listener, nullptr);
    }
}

void SpatialScene::SetListener(const Listener &l) {
    if (!state_->ok) {
        return;
    }
    ma_spatializer_listener *listener = &state_->listener;
    ma_spatializer_listener_set_position(listener, l.position.x, l.position.y, l.position.z);
    ma_spatializer_listener_set_velocity(listener, l.velocity.x, l.velocity.y, l.velocity.z);
    ma_spatializer_listener_set_direction(listener, l.forward.x, l.forward.y, l.forward.z);
    ma_spatializer_listener_set_world_up(listener, l.up.x, l.up.y, l.up.z);
    // Velocities are in game units per second; the speed of sound is in metres per second.
    ma_spatializer_listener_set_speed_of_sound(listener, kSpeedOfSound / l.distanceFactor);
    state_->rolloff = l.rolloffFactor;
    state_->doppler = l.dopplerFactor;
}

std::unique_ptr<SpatialScene::Source> SpatialScene::NewSource() {
    return std::make_unique<MaSource>(state_->channels, state_->sampleRate);
}

void SpatialScene::SetSource(Source &source, const Voice3D &p) {
    auto &s = static_cast<MaSource &>(source);
    if (!s.ok) {
        return;
    }
    ma_spatializer *sp = &s.spatializer;
    ma_spatializer_set_positioning(sp, p.space == VoiceSpace::kHeadRelative
                                           ? ma_positioning_relative
                                           : ma_positioning_absolute);
    ma_spatializer_set_position(sp, p.position.x, p.position.y, p.position.z);
    ma_spatializer_set_velocity(sp, p.velocity.x, p.velocity.y, p.velocity.z);
    ma_spatializer_set_direction(sp, p.coneOrientation.x, p.coneOrientation.y,
                                 p.coneOrientation.z);
    float minDistance = (std::max)(p.minDistance, kSmallestDistance);
    ma_spatializer_set_min_distance(sp, minDistance);
    ma_spatializer_set_max_distance(sp, (std::max)(p.maxDistance, minDistance));
    ma_spatializer_set_cone(sp, static_cast<float>(p.coneInsideDegrees) * kDegrees,
                            static_cast<float>(p.coneOutsideDegrees) * kDegrees,
                            p.coneOutsideGain);
}

void SpatialScene::Process(Source &source, const float *mono, float *out, std::size_t frames) {
    auto &s = static_cast<MaSource &>(source);
    if (!s.ok || !state_->ok) {
        std::fill(out, out + frames * state_->channels, 0.0f);
        return;
    }
    ma_spatializer_set_rolloff(&s.spatializer, state_->rolloff);
    ma_spatializer_set_doppler_factor(&s.spatializer, state_->doppler);
    ma_spatializer_process_pcm_frames(&s.spatializer, &state_->listener, out, mono, frames);
}

float SpatialScene::DopplerPitch(const Source &source) const {
    const auto &s = static_cast<const MaSource &>(source);
    return s.ok ? s.spatializer.dopplerPitch : 1.0f;
}

} // namespace sfcop::platform
