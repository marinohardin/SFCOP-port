// The spike's Miles self-test, check for check, against this module on the real mixer with a
// manual clock. The spike called each shim on a guest stack and counted two kinds of check: the
// result it asserted, and that the shim purged the bytes its name gives. Here each result is
// asserted on the module, in the same order and with the same expected value, and each call's
// purge is asserted on the shim source (MilesGuest.cpp) through ShimRows.h, since the shims run
// only with the lift. 19 result checks and 29 purge checks: 48 (README.md, "Self-test").
#include "hle/miles/Miles.h"
#include "hle/miles/ShimRows.h"

#include "platform/Audio/AudioMiniaudio.h"
#include "platform/Audio/ImaAdpcm.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace sfcop::hle::miles {
namespace {

using platform::AudioMiniaudio;
using platform::AudioOutput;

struct SelfTest {
    shimrows::Shims shims = shimrows::parseGuest(
        shimrows::readFile(shimrows::directoryOf(__FILE__) + "/MilesGuest.cpp"));
    int checks = 0;

    void ok(const char *what, bool cond) {
        ++checks;
        CAPTURE(what);
        CHECK(cond);
    }
    // The purge check of one call: the shim for `name` pops the @N its decorated name gives.
    void call(const std::string &name) {
        ++checks;
        CAPTURE(name);
        int bytes = std::stoi(name.substr(name.rfind('@') + 1));
        REQUIRE(shims.functionOf.count(name) == 1u);
        CHECK(shims.purgesOf[shims.functionOf[name]] == std::set<int>{bytes / 4});
    }
};

// Advances the mixer by `frames` output frames at 44100 Hz.
void advance(AudioMiniaudio &audio, std::size_t frames) {
    std::vector<float> out(frames * 2);
    REQUIRE(audio.Render(out.data(), frames).Ok());
}

} // namespace

TEST_CASE("the spike's Miles self-test passes against the module: 48 checks") {
    SelfTest t;

    // The hand-decoded IMA block: first sample 0, index 0, then 0x07: nibble 7 at step 7 gives
    // +11 (index 8), nibble 0 at step 16 gives +2.
    const std::uint8_t block[5] = {0x00, 0x00, 0x00, 0x00, 0x07};
    std::vector<std::int16_t> pcm = platform::decodeImaAdpcm(block, 5, 5, 1);
    t.ok("IMA block decodes to 3 frames", pcm.size() == 3);
    t.ok("IMA samples 0, 11, 13", pcm.size() == 3 && pcm[0] == 0 && pcm[1] == 11 && pcm[2] == 13);

    AudioMiniaudio audio(AudioOutput::kManualClock);
    Miles miles(audio);
    t.call("_AIL_waveOutOpen@16");
    t.ok("AIL_waveOutOpen", miles.WaveOutOpen() == 0);
    t.call("_AIL_allocate_sample_handle@4");
    std::uint32_t s = miles.AllocateSampleHandle();
    t.ok("a sample handle", s != 0);
    t.call("_AIL_init_sample@4");
    miles.InitSample(s);
    t.call("_AIL_sample_status@4");
    t.ok("a new sample is done", miles.SampleStatus(s) == kSmpDone);
    t.call("_AIL_set_sample_type@12");
    miles.SetSampleType(s, kDigF16Bits, 0); // 16-bit mono
    t.call("_AIL_set_sample_playback_rate@8");
    miles.SetSamplePlaybackRate(s, 22050);
    t.call("_AIL_set_sample_volume@8");
    miles.SetSampleVolume(s, 100);
    t.call("_AIL_sample_volume@4");
    t.ok("volume reads back", miles.SampleVolume(s) == 100);
    t.call("_AIL_set_sample_user_data@12");
    miles.SetSampleUserData(s, 3, 0xBEEF);
    t.call("_AIL_sample_buffer_ready@4");
    t.ok("buffer 0 is the one to fill", miles.SampleBufferReady(s) == 0);

    const std::vector<std::uint8_t> data(200, 0); // 100 frames of 16-bit mono
    t.call("_AIL_load_sample_buffer@16");
    miles.LoadSampleBuffer(s, 0, data.data(), 200);
    t.call("_AIL_sample_status@4");
    t.ok("loading starts the sample", miles.SampleStatus(s) == kSmpPlaying);
    t.call("_AIL_sample_buffer_ready@4");
    t.ok("then buffer 1", miles.SampleBufferReady(s) == 1);
    t.call("_AIL_load_sample_buffer@16");
    miles.LoadSampleBuffer(s, 1, data.data(), 200);
    t.call("_AIL_sample_buffer_ready@4");
    t.ok("both full", miles.SampleBufferReady(s) == -1);

    advance(audio, 300); // 150 source frames at 22050 Hz
    t.call("_AIL_sample_buffer_ready@4");
    t.ok("buffer 0 played out: fill it", miles.SampleBufferReady(s) == 0);
    t.call("_AIL_sample_status@4");
    t.ok("still playing", miles.SampleStatus(s) == kSmpPlaying);
    advance(audio, 200); // 100 more: past the end of buffer 1
    t.call("_AIL_sample_status@4");
    t.ok("starved: done", miles.SampleStatus(s) == kSmpDone);
    t.call("_AIL_load_sample_buffer@16");
    miles.LoadSampleBuffer(s, 0, data.data(), 200);
    t.call("_AIL_sample_status@4");
    t.ok("a load restarts it", miles.SampleStatus(s) == kSmpPlaying);
    t.call("_AIL_end_sample@4");
    miles.EndSample(s);
    t.call("_AIL_sample_status@4");
    t.ok("ended", miles.SampleStatus(s) == kSmpDone);

    t.call("_AIL_minimum_sample_buffer_size@12");
    t.ok("minimum buffer for 22 kHz 16-bit mono: 4 KB",
         miles.MinimumSampleBufferSize(22050, kDigF16Bits) == 4096);
    t.call("_AIL_set_redist_directory@4");
    t.ok("set_redist_directory: the previous one, empty",
         miles.SetRedistDirectory("redist").empty());
    t.call("_AIL_set_redist_directory@4");
    t.ok("set_redist_directory remembers", miles.SetRedistDirectory("other") == "redist");
    t.call("_AIL_release_sample_handle@4");
    miles.ReleaseSampleHandle(s);
    t.call("_AIL_sample_status@4");
    t.ok("a released handle reads as free", miles.SampleStatus(s) == kSmpFree);
    t.call("_AIL_serve@0");
    miles.Serve();
    t.call("_AIL_waveOutClose@4");
    miles.WaveOutClose();

    CHECK(t.checks == 48);
}

} // namespace sfcop::hle::miles
