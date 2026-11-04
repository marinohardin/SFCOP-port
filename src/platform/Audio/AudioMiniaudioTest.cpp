#include "platform/Audio/AudioMiniaudio.h"

#include <doctest/doctest.h>

#include <cmath>
#include <tuple>
#include <utility>
#include <vector>

namespace sfcop::platform {
namespace {

// A manual-clock backend open at 44.1 kHz stereo: time moves only in Render().
struct Rig {
    AudioMiniaudio audio{AudioOutput::kManualClock};
    Rig() { REQUIRE(audio.Open(OutputDesc{44100, 2}).Ok()); }

    // `frames` of a constant 16-bit value, mono or stereo.
    ClipId Constant(std::int16_t value, std::size_t frames, std::uint32_t rate = 22050,
                    std::uint8_t channels = 1) {
        std::vector<std::uint8_t> bytes;
        for (std::size_t i = 0; i < frames * channels; ++i) {
            bytes.push_back(static_cast<std::uint8_t>(static_cast<std::uint16_t>(value) & 0xFF));
            bytes.push_back(static_cast<std::uint8_t>(static_cast<std::uint16_t>(value) >> 8));
        }
        ClipFormat format;
        format.channels = channels;
        format.sampleRate = rate;
        Result<ClipId> clip = audio.LoadClip(format, bytes.data(), bytes.size());
        REQUIRE(clip.Ok());
        return clip.Value();
    }
    VoiceId Voice(ClipId clip) {
        Result<VoiceId> voice = audio.CreateVoice(clip);
        REQUIRE(voice.Ok());
        return voice.Value();
    }
    // One rendered stereo frame after `frames - 1` others.
    std::pair<float, float> FrameAfter(std::size_t frames) {
        std::vector<float> out(frames * 2);
        REQUIRE(audio.Render(out.data(), frames).Ok());
        return {out[out.size() - 2], out[out.size() - 1]};
    }
    std::uint64_t Frame(VoiceId voice) { return audio.GetStatus(voice).Value().frame; }
    std::size_t CompletionCount() {
        std::size_t n = 0;
        while (audio.PollCompletion().Value()) {
            ++n;
        }
        return n;
    }
};

constexpr std::int16_t kHalf = 16384;           // 0.5 full scale
constexpr float kCentre = 0.5f;                 // a mono voice at pan 0: unity on each side

} // namespace

TEST_CASE("opening the audio output checks the format and that it is not open already") {
    AudioMiniaudio audio{AudioOutput::kManualClock};
    CHECK(audio.Open(OutputDesc{4000, 2}).Code() == Error::kInvalidArgument);
    CHECK(audio.Open(OutputDesc{44100, 6}).Code() == Error::kInvalidArgument);
    CHECK(audio.Open(OutputDesc{44100, 2}).Ok());
    CHECK(audio.Open(OutputDesc{44100, 2}).Code() == Error::kWrongState);
    CHECK(audio.Close().Ok());
    CHECK(audio.Close().Ok());
    CHECK(audio.Open(OutputDesc{22050, 1}).Ok());
}

TEST_CASE("the null device opens a silent output without a sound card") {
    AudioMiniaudio audio{AudioOutput::kNullDevice};
    REQUIRE(audio.Open(OutputDesc{44100, 2}).Ok());
    CHECK(audio.Silent());
    float frame[2];
    CHECK(audio.Render(frame, 1).Code() == Error::kWrongState);
    CHECK(audio.Close().Ok());
}

TEST_CASE("rendering by hand needs an open manual-clock output") {
    AudioMiniaudio audio{AudioOutput::kManualClock};
    float frame[2];
    CHECK(audio.Render(frame, 1).Code() == Error::kWrongState);
}

TEST_CASE("closing frees every clip, voice and stream") {
    Rig rig;
    ClipId clip = rig.Constant(kHalf, 10);
    VoiceId voice = rig.Voice(clip);
    StreamId stream = rig.audio.OpenStream(ClipFormat{}).Value();
    CHECK(rig.audio.Close().Ok());
    CHECK(rig.audio.GetStatus(voice).Code() == Error::kInvalidHandle);
    CHECK(rig.audio.ReleaseClip(clip).Code() == Error::kInvalidHandle);
    CHECK(rig.audio.QueuedBuffers(stream).Code() == Error::kInvalidHandle);
}

TEST_CASE("a clip needs an open output, a known format and whole frames or blocks") {
    AudioMiniaudio closed{AudioOutput::kManualClock};
    std::uint8_t bytes[64] = {};
    CHECK(closed.LoadClip(ClipFormat{}, bytes, 4).Code() == Error::kWrongState);

    Rig rig;
    ClipFormat stereo3;
    stereo3.channels = 3;
    CHECK(rig.audio.LoadClip(stereo3, bytes, 6).Code() == Error::kInvalidArgument);
    ClipFormat slow;
    slow.sampleRate = 500;
    CHECK(rig.audio.LoadClip(slow, bytes, 4).Code() == Error::kInvalidArgument);
    CHECK(rig.audio.LoadClip(ClipFormat{}, bytes, 0).Code() == Error::kInvalidArgument);
    CHECK(rig.audio.LoadClip(ClipFormat{}, nullptr, 4).Code() == Error::kInvalidArgument);
    CHECK(rig.audio.LoadClip(ClipFormat{}, bytes, 3).Code() == Error::kInvalidArgument);
    ClipFormat adpcm;
    adpcm.format = SampleFormat::kImaAdpcm;
    CHECK(rig.audio.LoadClip(adpcm, bytes, 32).Code() == Error::kInvalidArgument);
    adpcm.adpcmBlockBytes = 32;
    CHECK(rig.audio.LoadClip(adpcm, bytes, 48).Code() == Error::kInvalidArgument);
    CHECK(rig.audio.LoadClip(adpcm, bytes, 64).Ok());
    ClipFormat pcm8;
    pcm8.format = SampleFormat::kPcm8;
    CHECK(rig.audio.LoadClip(pcm8, bytes, 3).Ok());
}

TEST_CASE("an 8-bit clip plays 128 as silence and an ADPCM clip plays decoded") {
    Rig rig;
    ClipFormat pcm8;
    pcm8.format = SampleFormat::kPcm8;
    pcm8.sampleRate = 44100;
    std::vector<std::uint8_t> eight(100, 192); // +64 / 128 = 0.5
    VoiceId voice = rig.Voice(rig.audio.LoadClip(pcm8, eight.data(), eight.size()).Value());
    REQUIRE(rig.audio.Play(voice, true).Ok());
    auto [l, r] = rig.FrameAfter(10);
    CHECK(l == doctest::Approx(kCentre));
    CHECK(r == doctest::Approx(kCentre));
    REQUIRE(rig.audio.DestroyVoice(voice).Ok());

    // One mono block: header sample 16384, index 0, then zero nibbles (each adds 7 >> 3 = 0).
    std::vector<std::uint8_t> block = {0x00, 0x40, 0, 0};
    block.insert(block.end(), 12, 0x00);
    ClipFormat adpcm;
    adpcm.format = SampleFormat::kImaAdpcm;
    adpcm.adpcmBlockBytes = 16;
    adpcm.sampleRate = 44100;
    voice = rig.Voice(rig.audio.LoadClip(adpcm, block.data(), block.size()).Value());
    REQUIRE(rig.audio.Play(voice, true).Ok());
    std::tie(l, r) = rig.FrameAfter(5);
    CHECK(l == doctest::Approx(kCentre));
}

TEST_CASE("writing into a playing PCM clip is heard from the next mix") {
    Rig rig;
    ClipId clip = rig.Constant(0, 100, 44100);
    VoiceId voice = rig.Voice(clip);
    REQUIRE(rig.audio.Play(voice, true).Ok());
    CHECK(rig.FrameAfter(10).first == 0.0f);
    std::vector<std::uint8_t> half(200);
    for (std::size_t i = 0; i < half.size(); i += 2) {
        half[i] = 0x00;
        half[i + 1] = 0x40;
    }
    REQUIRE(rig.audio.WriteClip(clip, 0, half.data(), half.size()).Ok());
    CHECK(rig.FrameAfter(10).first == doctest::Approx(kCentre));

    CHECK(rig.audio.WriteClip(clip, 150, half.data(), 51).Code() == Error::kInvalidArgument);
    CHECK(rig.audio.WriteClip(clip, 201, half.data(), 0).Code() == Error::kInvalidArgument);
    CHECK(rig.audio.WriteClip(ClipId{999}, 0, half.data(), 2).Code() == Error::kInvalidHandle);
    std::vector<std::uint8_t> block(16, 0);
    ClipFormat adpcm;
    adpcm.format = SampleFormat::kImaAdpcm;
    adpcm.adpcmBlockBytes = 16;
    ClipId compressed = rig.audio.LoadClip(adpcm, block.data(), block.size()).Value();
    CHECK(rig.audio.WriteClip(compressed, 0, block.data(), 2).Code() ==
          Error::kInvalidArgument);
}

TEST_CASE("releasing a clip destroys its voices") {
    Rig rig;
    ClipId clip = rig.Constant(kHalf, 10);
    VoiceId voice = rig.Voice(clip);
    CHECK(rig.audio.ReleaseClip(clip).Ok());
    CHECK(rig.audio.GetStatus(voice).Code() == Error::kInvalidHandle);
    CHECK(rig.audio.ReleaseClip(clip).Code() == Error::kInvalidHandle);
    CHECK(rig.audio.CreateVoice(clip).Code() == Error::kInvalidHandle);
}

TEST_CASE("a voice starts stopped at frame 0 and the voice limit is at least 64") {
    Rig rig;
    ClipId clip = rig.Constant(kHalf, 10);
    VoiceId first = rig.Voice(clip);
    VoiceStatus status = rig.audio.GetStatus(first).Value();
    CHECK(status.state == VoiceState::kStopped);
    CHECK(status.frame == 0);
    int made = 1;
    while (rig.audio.CreateVoice(clip).Ok()) {
        ++made;
        REQUIRE(made < 10000);
    }
    CHECK(made >= 64);
    CHECK(rig.audio.CreateVoice(clip).Code() == Error::kOutOfResources);
    CHECK(rig.audio.DestroyVoice(first).Ok());
    CHECK(rig.audio.DestroyVoice(first).Code() == Error::kInvalidHandle);
    CHECK(rig.audio.CreateVoice(clip).Ok());
}

TEST_CASE("a voice advances at its rate, stops in place and moves when told") {
    Rig rig;
    VoiceId voice = rig.Voice(rig.Constant(kHalf, 100)); // 22050 Hz: half a frame per output frame
    REQUIRE(rig.audio.Play(voice, false).Ok());
    rig.FrameAfter(100);
    CHECK(rig.Frame(voice) == 50);
    CHECK(rig.audio.GetStatus(voice).Value().state == VoiceState::kPlaying);
    REQUIRE(rig.audio.Stop(voice).Ok());
    rig.FrameAfter(100);
    CHECK(rig.Frame(voice) == 50);
    CHECK(rig.audio.GetStatus(voice).Value().state == VoiceState::kStopped);
    CHECK(rig.audio.SetPosition(voice, 101).Code() == Error::kInvalidArgument);
    CHECK(rig.audio.SetPosition(voice, 10).Ok());
    CHECK(rig.Frame(voice) == 10);
    REQUIRE(rig.audio.SetRate(voice, 44100).Ok());
    REQUIRE(rig.audio.Play(voice, false).Ok());
    rig.FrameAfter(30);
    CHECK(rig.Frame(voice) == 40);

    VoiceId gone{12345};
    CHECK(rig.audio.Play(gone, false).Code() == Error::kInvalidHandle);
    CHECK(rig.audio.Stop(gone).Code() == Error::kInvalidHandle);
    CHECK(rig.audio.SetPosition(gone, 0).Code() == Error::kInvalidHandle);
    CHECK(rig.audio.GetStatus(gone).Code() == Error::kInvalidHandle);
}

TEST_CASE("a voice played to its end appears once in the completion queue") {
    Rig rig;
    VoiceId voice = rig.Voice(rig.Constant(kHalf, 100, 44100));
    REQUIRE(rig.audio.Play(voice, false).Ok());
    rig.FrameAfter(99);
    CHECK(rig.CompletionCount() == 0);
    rig.FrameAfter(50);
    Result<std::optional<Completion>> done = rig.audio.PollCompletion();
    REQUIRE(done.Ok());
    REQUIRE(done.Value().has_value());
    CHECK(done.Value()->kind == CompletionKind::kVoiceEnded);
    CHECK(done.Value()->voice == voice);
    CHECK(!rig.audio.PollCompletion().Value().has_value());
    rig.FrameAfter(100);
    CHECK(rig.CompletionCount() == 0);
    VoiceStatus status = rig.audio.GetStatus(voice).Value();
    CHECK(status.state == VoiceState::kStopped);
    CHECK(status.frame == 100);
    CHECK(rig.FrameAfter(1).first == 0.0f);
}

TEST_CASE("a looping voice wraps without completing and stop queues nothing") {
    Rig rig;
    VoiceId voice = rig.Voice(rig.Constant(kHalf, 100, 44100));
    REQUIRE(rig.audio.Play(voice, true).Ok());
    rig.FrameAfter(250);
    CHECK(rig.Frame(voice) == 50);
    REQUIRE(rig.audio.Stop(voice).Ok());
    CHECK(rig.CompletionCount() == 0);
    // Playing a playing voice only changes the loop flag: it now ends at frame 100.
    REQUIRE(rig.audio.Play(voice, true).Ok());
    REQUIRE(rig.audio.Play(voice, false).Ok());
    rig.FrameAfter(60);
    CHECK(rig.CompletionCount() == 1);
}

TEST_CASE("a completion of a destroyed voice is removed from the queue") {
    Rig rig;
    VoiceId voice = rig.Voice(rig.Constant(kHalf, 10, 44100));
    REQUIRE(rig.audio.Play(voice, false).Ok());
    rig.FrameAfter(20);
    REQUIRE(rig.audio.DestroyVoice(voice).Ok());
    CHECK(rig.CompletionCount() == 0);
}

TEST_CASE("gain scales a voice and pan attenuates one side as DirectSound does") {
    Rig rig;
    VoiceId voice = rig.Voice(rig.Constant(kHalf, 100, 44100));
    REQUIRE(rig.audio.Play(voice, true).Ok());
    auto [l, r] = rig.FrameAfter(5);
    CHECK(l == doctest::Approx(kCentre));
    CHECK(r == doctest::Approx(kCentre));
    REQUIRE(rig.audio.SetGain(voice, 0.5f).Ok());
    std::tie(l, r) = rig.FrameAfter(5);
    CHECK(l == doctest::Approx(kCentre * 0.5f));
    REQUIRE(rig.audio.SetGain(voice, 1.0f).Ok());
    REQUIRE(rig.audio.SetPan(voice, -1.0f).Ok());
    std::tie(l, r) = rig.FrameAfter(5);
    CHECK(l == doctest::Approx(0.5f));
    CHECK(r == doctest::Approx(0.0f));
    REQUIRE(rig.audio.SetPan(voice, 1.0f).Ok());
    std::tie(l, r) = rig.FrameAfter(5);
    CHECK(l == doctest::Approx(0.0f));
    CHECK(r == doctest::Approx(0.5f));
    REQUIRE(rig.audio.SetPan(voice, 0.5f).Ok());
    std::tie(l, r) = rig.FrameAfter(5);
    // pan +0.5 is 5000 hundredths of a decibel: the left at 10^(-2.5) = 0.0031623, the right
    // at unity.
    CHECK(l == doctest::Approx(0.5f * 0.0031623f).epsilon(0.001));
    CHECK(r == doctest::Approx(0.5f));

    CHECK(rig.audio.SetGain(voice, 1.5f).Code() == Error::kInvalidArgument);
    CHECK(rig.audio.SetGain(voice, -0.1f).Code() == Error::kInvalidArgument);
    CHECK(rig.audio.SetGain(voice, std::nanf("")).Code() == Error::kInvalidArgument);
    CHECK(rig.audio.SetPan(voice, 1.1f).Code() == Error::kInvalidArgument);
    CHECK(rig.audio.SetRate(voice, 99).Code() == Error::kInvalidArgument);
    CHECK(rig.audio.SetRate(voice, 192001).Code() == Error::kInvalidArgument);
    VoiceId gone{12345};
    CHECK(rig.audio.SetGain(gone, 1.0f).Code() == Error::kInvalidHandle);
    CHECK(rig.audio.SetPan(gone, 0.0f).Code() == Error::kInvalidHandle);
    CHECK(rig.audio.SetRate(gone, 22050).Code() == Error::kInvalidHandle);
}

TEST_CASE("a stereo voice keeps both sides at unity in the centre") {
    Rig rig;
    VoiceId voice = rig.Voice(rig.Constant(kHalf, 100, 44100, 2));
    REQUIRE(rig.audio.Play(voice, true).Ok());
    auto [l, r] = rig.FrameAfter(5);
    CHECK(l == doctest::Approx(0.5f));
    CHECK(r == doctest::Approx(0.5f));
}

TEST_CASE("the master gain scales the whole mix") {
    Rig rig;
    VoiceId voice = rig.Voice(rig.Constant(kHalf, 100, 44100));
    REQUIRE(rig.audio.Play(voice, true).Ok());
    REQUIRE(rig.audio.SetMasterGain(0.25f).Ok());
    CHECK(rig.FrameAfter(5).first == doctest::Approx(kCentre * 0.25f));
    CHECK(rig.audio.SetMasterGain(2.0f).Code() == Error::kInvalidArgument);
}

TEST_CASE("3D parameters and the listener are checked") {
    Rig rig;
    VoiceId voice = rig.Voice(rig.Constant(kHalf, 100));
    Voice3D p;
    p.space = VoiceSpace::kWorld;
    CHECK(rig.audio.SetVoice3D(voice, p).Ok());
    Voice3D bad = p;
    bad.minDistance = -1.0f;
    CHECK(rig.audio.SetVoice3D(voice, bad).Code() == Error::kInvalidArgument);
    bad = p;
    bad.minDistance = 10.0f;
    bad.maxDistance = 5.0f;
    CHECK(rig.audio.SetVoice3D(voice, bad).Code() == Error::kInvalidArgument);
    bad = p;
    bad.coneInsideDegrees = 200;
    bad.coneOutsideDegrees = 100;
    CHECK(rig.audio.SetVoice3D(voice, bad).Code() == Error::kInvalidArgument);
    bad = p;
    bad.coneInsideDegrees = 361;
    bad.coneOutsideDegrees = 361;
    CHECK(rig.audio.SetVoice3D(voice, bad).Code() == Error::kInvalidArgument);
    bad = p;
    bad.coneOutsideGain = 1.5f;
    CHECK(rig.audio.SetVoice3D(voice, bad).Code() == Error::kInvalidArgument);
    CHECK(rig.audio.SetVoice3D(VoiceId{12345}, p).Code() == Error::kInvalidHandle);

    Listener l;
    CHECK(rig.audio.SetListener(l).Ok());
    Listener badL = l;
    badL.forward = Vector3{};
    CHECK(rig.audio.SetListener(badL).Code() == Error::kInvalidArgument);
    badL = l;
    badL.up = Vector3{};
    CHECK(rig.audio.SetListener(badL).Code() == Error::kInvalidArgument);
    badL = l;
    badL.distanceFactor = 0.0f;
    CHECK(rig.audio.SetListener(badL).Code() == Error::kInvalidArgument);
    badL = l;
    badL.rolloffFactor = 11.0f;
    CHECK(rig.audio.SetListener(badL).Code() == Error::kInvalidArgument);
    badL = l;
    badL.dopplerFactor = -1.0f;
    CHECK(rig.audio.SetListener(badL).Code() == Error::kInvalidArgument);
}

namespace {

// The average left and right level of a 3D voice of constant 0.5 at `where`, once the
// spatializer's gain smoothing has settled.
std::pair<float, float> levelsAt(Rig &rig, Vector3 where, float minDistance = 10.0f,
                                 Vector3 facing = Vector3{0.0f, 0.0f, 1.0f},
                                 std::uint32_t insideDegrees = 360,
                                 std::uint32_t outsideDegrees = 360) {
    VoiceId voice = rig.Voice(rig.Constant(kHalf, 1000, 44100));
    Voice3D p;
    p.space = VoiceSpace::kWorld;
    p.position = where;
    p.minDistance = minDistance;
    p.coneOrientation = facing;
    p.coneInsideDegrees = insideDegrees;
    p.coneOutsideDegrees = outsideDegrees;
    p.coneOutsideGain = 0.25f;
    REQUIRE(rig.audio.SetVoice3D(voice, p).Ok());
    REQUIRE(rig.audio.Play(voice, true).Ok());
    rig.FrameAfter(2048);
    std::vector<float> out(256 * 2);
    REQUIRE(rig.audio.Render(out.data(), 256).Ok());
    float l = 0.0f;
    float r = 0.0f;
    for (std::size_t i = 0; i < 256; ++i) {
        l += out[2 * i];
        r += out[2 * i + 1];
    }
    REQUIRE(rig.audio.DestroyVoice(voice).Ok());
    return {l / 256.0f, r / 256.0f};
}

float total(std::pair<float, float> lr) { return lr.first + lr.second; }

} // namespace

TEST_CASE("3D attenuation follows distance and pan follows the angle to the listener") {
    Rig rig;
    // Listener at the origin facing +z with +y up (the defaults); minimum distance 10, rolloff 1.
    auto ahead20 = levelsAt(rig, Vector3{0.0f, 0.0f, 20.0f});
    auto ahead40 = levelsAt(rig, Vector3{0.0f, 0.0f, 40.0f});
    auto ahead5 = levelsAt(rig, Vector3{0.0f, 0.0f, 5.0f});
    auto right20 = levelsAt(rig, Vector3{20.0f, 0.0f, 0.0f});
    auto left20 = levelsAt(rig, Vector3{-20.0f, 0.0f, 0.0f});

    // miniaudio's inverse distance model: gain min / (min + rolloff (d - min)), so 1/2 at 20, 1/4
    // at 40 and full inside the minimum distance. At rolloff 1 this is DirectSound3D's law; at
    // another rolloff the two differ and only this model is checked.
    CHECK(total(ahead20) / total(ahead40) == doctest::Approx(2.0f).epsilon(0.01));
    CHECK(total(ahead5) / total(ahead20) == doctest::Approx(2.0f).epsilon(0.01));
    // Straight ahead is centred; to the right is louder on the right, to the left on the left,
    // mirror images of each other, at the same distance gain.
    CHECK(ahead20.first == doctest::Approx(ahead20.second));
    CHECK(right20.second > right20.first);
    CHECK(left20.first > left20.second);
    CHECK(right20.second == doctest::Approx(left20.first));
    CHECK(right20.first == doctest::Approx(left20.second));

    // Rolloff 0 turns distance attenuation off.
    Listener noRolloff;
    noRolloff.rolloffFactor = 0.0f;
    REQUIRE(rig.audio.SetListener(noRolloff).Ok());
    CHECK(total(levelsAt(rig, Vector3{0.0f, 0.0f, 40.0f})) ==
          doctest::Approx(total(ahead5)).epsilon(0.01));
    REQUIRE(rig.audio.SetListener(Listener{}).Ok());

    // A listener turned to face +x hears a source at +z on its left.
    Listener turned;
    turned.forward = Vector3{1.0f, 0.0f, 0.0f};
    REQUIRE(rig.audio.SetListener(turned).Ok());
    auto side = levelsAt(rig, Vector3{0.0f, 0.0f, 20.0f});
    CHECK(side.first > side.second);
    REQUIRE(rig.audio.SetListener(Listener{}).Ok());

    // A source facing away from the listener, with a 90 degree inner and 180 degree outer cone,
    // plays at the cone's outside gain (0.25).
    auto away =
        levelsAt(rig, Vector3{0.0f, 0.0f, 20.0f}, 10.0f, Vector3{0.0f, 0.0f, 1.0f}, 90, 180);
    auto toward =
        levelsAt(rig, Vector3{0.0f, 0.0f, 20.0f}, 10.0f, Vector3{0.0f, 0.0f, -1.0f}, 90, 180);
    CHECK(total(away) / total(toward) == doctest::Approx(0.25f).epsilon(0.01));
}

TEST_CASE("a head-relative voice is placed relative to the listener whatever its position") {
    Rig rig;
    Listener moved;
    moved.position = Vector3{500.0f, 0.0f, 500.0f};
    REQUIRE(rig.audio.SetListener(moved).Ok());
    VoiceId voice = rig.Voice(rig.Constant(kHalf, 1000, 44100));
    Voice3D p;
    p.space = VoiceSpace::kHeadRelative;
    p.position = Vector3{20.0f, 0.0f, 0.0f};
    p.minDistance = 10.0f;
    REQUIRE(rig.audio.SetVoice3D(voice, p).Ok());
    REQUIRE(rig.audio.Play(voice, true).Ok());
    auto [l, r] = rig.FrameAfter(2048);
    CHECK(r > l);
    CHECK(r > 0.0f);
}

TEST_CASE("a stream plays its buffers in order and reports each one done") {
    Rig rig;
    ClipFormat format; // 16-bit mono 22050 Hz
    StreamId stream = rig.audio.OpenStream(format).Value();
    std::vector<std::uint8_t> buffer(200); // 100 frames of silence
    REQUIRE(rig.audio.QueueBuffer(stream, buffer.data(), buffer.size()).Ok());
    REQUIRE(rig.audio.QueueBuffer(stream, buffer.data(), buffer.size()).Ok());
    CHECK(rig.audio.QueuedBuffers(stream).Value() == 2);
    rig.FrameAfter(300); // nothing moves while stopped
    CHECK(rig.audio.QueuedBuffers(stream).Value() == 2);
    REQUIRE(rig.audio.PlayStream(stream).Ok());
    rig.FrameAfter(300); // 150 source frames: the first buffer is done
    CHECK(rig.audio.QueuedBuffers(stream).Value() == 1);
    Result<std::optional<Completion>> done = rig.audio.PollCompletion();
    REQUIRE(done.Value().has_value());
    CHECK(done.Value()->kind == CompletionKind::kStreamBufferDone);
    CHECK(done.Value()->stream == stream);
    REQUIRE(rig.audio.StopStream(stream).Ok());
    rig.FrameAfter(300);
    CHECK(rig.audio.QueuedBuffers(stream).Value() == 1);
    REQUIRE(rig.audio.PlayStream(stream).Ok());
    rig.FrameAfter(200); // past the end: starved, still playing, silent
    CHECK(rig.audio.QueuedBuffers(stream).Value() == 0);
    CHECK(rig.CompletionCount() == 1);
    std::vector<std::uint8_t> half(200);
    for (std::size_t i = 0; i < half.size(); i += 2) {
        half[i + 1] = 0x40;
    }
    REQUIRE(rig.audio.QueueBuffer(stream, half.data(), half.size()).Ok());
    CHECK(rig.FrameAfter(4).first == doctest::Approx(kCentre));
}

TEST_CASE("a stream's queue holds 16 buffers and flushing empties it silently") {
    Rig rig;
    StreamId stream = rig.audio.OpenStream(ClipFormat{}).Value();
    std::uint8_t buffer[8] = {};
    for (int i = 0; i < 16; ++i) {
        REQUIRE(rig.audio.QueueBuffer(stream, buffer, sizeof buffer).Ok());
    }
    CHECK(rig.audio.QueueBuffer(stream, buffer, sizeof buffer).Code() == Error::kOutOfResources);
    CHECK(rig.audio.QueueBuffer(stream, buffer, 3).Code() == Error::kInvalidArgument);
    CHECK(rig.audio.QueueBuffer(stream, buffer, 0).Code() == Error::kInvalidArgument);
    REQUIRE(rig.audio.FlushStream(stream).Ok());
    CHECK(rig.audio.QueuedBuffers(stream).Value() == 0);
    CHECK(rig.CompletionCount() == 0);
}

TEST_CASE("a stream's gain and pan are checked and applied") {
    Rig rig;
    ClipFormat format;
    format.sampleRate = 44100;
    StreamId stream = rig.audio.OpenStream(format).Value();
    std::vector<std::uint8_t> half(2000);
    for (std::size_t i = 0; i < half.size(); i += 2) {
        half[i + 1] = 0x40;
    }
    REQUIRE(rig.audio.QueueBuffer(stream, half.data(), half.size()).Ok());
    REQUIRE(rig.audio.PlayStream(stream).Ok());
    REQUIRE(rig.audio.SetStreamGain(stream, 0.5f).Ok());
    REQUIRE(rig.audio.SetStreamPan(stream, 1.0f).Ok());
    auto [l, r] = rig.FrameAfter(4);
    CHECK(l == doctest::Approx(0.0f));
    CHECK(r == doctest::Approx(0.25f));
    CHECK(rig.audio.SetStreamGain(stream, 1.01f).Code() == Error::kInvalidArgument);
    CHECK(rig.audio.SetStreamPan(stream, -1.01f).Code() == Error::kInvalidArgument);
}

TEST_CASE("opening a stream needs an open output and a known format; closing frees it") {
    AudioMiniaudio closed{AudioOutput::kManualClock};
    CHECK(closed.OpenStream(ClipFormat{}).Code() == Error::kWrongState);
    Rig rig;
    ClipFormat bad;
    bad.channels = 0;
    CHECK(rig.audio.OpenStream(bad).Code() == Error::kInvalidArgument);
    StreamId stream = rig.audio.OpenStream(ClipFormat{}).Value();
    std::uint8_t buffer[4] = {};
    REQUIRE(rig.audio.QueueBuffer(stream, buffer, sizeof buffer).Ok());
    REQUIRE(rig.audio.PlayStream(stream).Ok());
    rig.FrameAfter(10);
    REQUIRE(rig.audio.CloseStream(stream).Ok());
    CHECK(rig.CompletionCount() == 0);
    CHECK(rig.audio.CloseStream(stream).Code() == Error::kInvalidHandle);
    CHECK(rig.audio.QueueBuffer(stream, buffer, 4).Code() == Error::kInvalidHandle);
    CHECK(rig.audio.QueuedBuffers(stream).Code() == Error::kInvalidHandle);
    CHECK(rig.audio.PlayStream(stream).Code() == Error::kInvalidHandle);
    CHECK(rig.audio.StopStream(stream).Code() == Error::kInvalidHandle);
    CHECK(rig.audio.FlushStream(stream).Code() == Error::kInvalidHandle);
    CHECK(rig.audio.SetStreamGain(stream, 1.0f).Code() == Error::kInvalidHandle);
    CHECK(rig.audio.SetStreamPan(stream, 0.0f).Code() == Error::kInvalidHandle);
}

TEST_CASE("makeAudio returns the miniaudio backend") {
    std::unique_ptr<Audio> audio = makeAudio();
    REQUIRE(audio != nullptr);
    CHECK(dynamic_cast<AudioMiniaudio *>(audio.get()) != nullptr);
    CHECK(audio->PollCompletion().Ok());
}

TEST_CASE("sixty-four streams play at once") {
    Rig rig;
    ClipFormat format;
    format.sampleRate = 44100;
    std::vector<std::uint8_t> half(2000);
    for (std::size_t i = 0; i < half.size(); i += 2) {
        half[i + 1] = 0x04; // 1024 / 32768 = 1/32 each
    }
    std::vector<StreamId> streams;
    for (int i = 0; i < 64; ++i) {
        Result<StreamId> opened = rig.audio.OpenStream(format);
        REQUIRE(opened.Ok());
        REQUIRE(rig.audio.QueueBuffer(opened.Value(), half.data(), half.size()).Ok());
        REQUIRE(rig.audio.PlayStream(opened.Value()).Ok());
        streams.push_back(opened.Value());
    }
    auto [l, r] = rig.FrameAfter(4);
    CHECK(l == doctest::Approx(64.0f / 32.0f).epsilon(0.001)); // all sixty-four are heard
    CHECK(r == doctest::Approx(64.0f / 32.0f).epsilon(0.001));
    // Past the limit the answer is the documented error, never silence.
    CHECK(rig.audio.OpenStream(format).Code() == Error::kOutOfResources);
}

TEST_CASE("DirectSound's pan law: unity at the centre, the far side down by the pan in dB") {
    Rig rig;
    VoiceId mono = rig.Voice(rig.Constant(kHalf, 100, 44100));
    VoiceId stereo = rig.Voice(rig.Constant(kHalf, 100, 44100, 2));
    // Centre: a mono source is 0.5 on each side, unity gain on each.
    REQUIRE(rig.audio.Play(mono, true).Ok());
    auto [l, r] = rig.FrameAfter(5);
    CHECK(l == doctest::Approx(0.5f).epsilon(1e-4));
    CHECK(r == doctest::Approx(0.5f).epsilon(1e-4));
    // DirectSound pan -2000 (20 dB down on the right) is -0.2 in the Audio scale: right over
    // left is 0.1.
    REQUIRE(rig.audio.SetPan(mono, -0.2f).Ok());
    std::tie(l, r) = rig.FrameAfter(5);
    CHECK(l == doctest::Approx(0.5f).epsilon(1e-4));
    CHECK(r / l == doctest::Approx(0.1f).epsilon(1e-3));
    REQUIRE(rig.audio.Stop(mono).Ok());
    // The same for a stereo source: each channel scaled by its side's factor.
    REQUIRE(rig.audio.Play(stereo, true).Ok());
    REQUIRE(rig.audio.SetPan(stereo, -0.2f).Ok());
    std::tie(l, r) = rig.FrameAfter(5);
    CHECK(l == doctest::Approx(0.5f).epsilon(1e-4));
    CHECK(r / l == doctest::Approx(0.1f).epsilon(1e-3));
    REQUIRE(rig.audio.SetPan(stereo, 0.2f).Ok());
    std::tie(l, r) = rig.FrameAfter(5);
    CHECK(r == doctest::Approx(0.5f).epsilon(1e-4));
    CHECK(l / r == doctest::Approx(0.1f).epsilon(1e-3));
}

} // namespace sfcop::platform
