#include "hle/miles/Miles.h"

#include "platform/Audio/AudioMock.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace sfcop::hle::miles {
namespace {

using platform::AudioMock;
using platform::Error;
using platform::StreamId;
using Calls = std::vector<std::string>;

struct Rig {
    AudioMock audio;
    Miles miles{audio};
    std::uint32_t sample = 0;

    Rig() {
        REQUIRE(miles.WaveOutOpen() == 0);
        sample = miles.AllocateSampleHandle();
        REQUIRE(sample != 0);
        audio.log.Clear();
    }
    // Loads 100 frames of 16-bit mono silence as `buffer`.
    void Load(std::uint32_t buffer) {
        std::vector<std::uint8_t> data(200, 0);
        miles.LoadSampleBuffer(sample, buffer, data.data(), 200);
    }
};

} // namespace

TEST_CASE("AIL_startup succeeds") {
    AudioMock audio;
    Miles miles{audio};
    CHECK(miles.Startup() == 1);
    CHECK(audio.log.Calls().empty());
}

TEST_CASE("AIL_waveOutOpen opens 44.1 kHz stereo once at the master volume") {
    AudioMock audio;
    Miles miles{audio};
    miles.SetDigitalMasterVolume(0);
    CHECK(miles.WaveOutOpen() == 0);
    CHECK(miles.WaveOutOpen() == 0);
    CHECK(audio.log.Calls() == Calls{"Open(44100, 2)", "SetMasterGain(0)"});
    CHECK(miles.LastError().empty());
}

TEST_CASE("AIL_waveOutOpen reports an output that does not open") {
    AudioMock audio;
    audio.failWith = Error::kIoError;
    Miles miles{audio};
    CHECK(miles.WaveOutOpen() == 1);
    CHECK(miles.LastError() == "no digital output device");
}

TEST_CASE("AIL_waveOutOpen accepts an output something else opened") {
    AudioMock audio;
    audio.failWith = Error::kWrongState;
    Miles miles{audio};
    CHECK(miles.WaveOutOpen() == 0);
}

TEST_CASE("AIL_waveOutClose stops every sample") {
    Rig rig;
    rig.Load(0);
    rig.audio.log.Clear();
    rig.miles.WaveOutClose();
    CHECK(rig.audio.log.Calls() == Calls{"StopStream(1)", "FlushStream(1)"});
    rig.audio.queuedBuffersResult = std::size_t{0};
    CHECK(rig.miles.SampleStatus(rig.sample) == kSmpDone);
}

TEST_CASE("AIL_allocate_sample_handle hands out 64 handles, then none") {
    AudioMock audio;
    Miles miles{audio};
    std::vector<std::uint32_t> handles;
    for (std::uint32_t i = 0; i < kMaxSamples; ++i) {
        handles.push_back(miles.AllocateSampleHandle());
    }
    CHECK(handles.front() == 1);
    CHECK(handles.back() == 64);
    CHECK(miles.AllocateSampleHandle() == 0);
    CHECK(miles.LastError() == "out of sample handles");
    miles.ReleaseSampleHandle(7);
    CHECK(miles.AllocateSampleHandle() == 7);
}

TEST_CASE("AIL_release_sample_handle closes the sample's stream and frees the handle") {
    Rig rig;
    rig.Load(0);
    rig.audio.log.Clear();
    rig.miles.ReleaseSampleHandle(rig.sample);
    CHECK(rig.audio.log.Calls() == Calls{"CloseStream(1)"});
    CHECK(rig.miles.SampleStatus(rig.sample) == kSmpFree);
    rig.miles.ReleaseSampleHandle(rig.sample); // a dead handle is ignored
    rig.miles.ReleaseSampleHandle(0);
    rig.miles.ReleaseSampleHandle(65);
}

TEST_CASE("AIL_init_sample stops the sample and restores the defaults") {
    Rig rig;
    rig.miles.SetSampleVolume(rig.sample, 10);
    rig.miles.SetSampleUserData(rig.sample, 2, 77);
    rig.Load(0);
    rig.audio.log.Clear();
    rig.miles.InitSample(rig.sample);
    CHECK(rig.audio.log.Calls() == Calls{"StopStream(1)", "FlushStream(1)"});
    CHECK(rig.miles.SampleVolume(rig.sample) == 127);
    CHECK(rig.miles.SampleUserData(rig.sample, 2) == 77);
    CHECK(rig.miles.SampleBufferReady(rig.sample) == 0);
}

TEST_CASE("AIL_end_sample stops the sample and discards its buffers") {
    Rig rig;
    rig.Load(0);
    rig.audio.log.Clear();
    rig.miles.EndSample(rig.sample);
    CHECK(rig.audio.log.Calls() == Calls{"StopStream(1)", "FlushStream(1)"});
    rig.audio.queuedBuffersResult = std::size_t{0};
    CHECK(rig.miles.SampleStatus(rig.sample) == kSmpDone);
    rig.miles.EndSample(99);
}

TEST_CASE("AIL_set_sample_type and AIL_set_sample_playback_rate set the stream's format") {
    Rig rig;
    rig.miles.SetSampleType(rig.sample, kDigF16Bits | kDigFStereo, 0);
    rig.miles.SetSamplePlaybackRate(rig.sample, 11025);
    std::vector<std::uint8_t> data(8, 0);
    rig.miles.LoadSampleBuffer(rig.sample, 0, data.data(), 8);
    CHECK(rig.audio.log.Calls() == Calls{"OpenStream(1, 2, 11025)", "SetStreamGain(1, 1)",
                                         "SetStreamPan(1, 0)", "QueueBuffer(1, 8)",
                                         "PlayStream(1)"});
}

TEST_CASE("a new rate reopens the sample's stream at the next load") {
    Rig rig;
    rig.Load(0);
    rig.audio.log.Clear();
    rig.audio.openStreamResult = StreamId{2};
    rig.miles.SetSamplePlaybackRate(rig.sample, 44100);
    rig.Load(1);
    CHECK(rig.audio.log.Calls() == Calls{"CloseStream(1)", "OpenStream(1, 1, 44100)",
                                         "SetStreamGain(2, 1)", "SetStreamPan(2, 0)",
                                         "QueueBuffer(2, 200)", "PlayStream(2)"});
}

TEST_CASE("AIL_set_sample_adpcm_block_size decodes loads in blocks of that size") {
    Rig rig;
    rig.miles.SetSampleType(rig.sample, kDigFAdpcm, 0);
    rig.miles.SetSampleAdpcmBlockSize(rig.sample, 5);
    // Two 5-byte mono blocks: 3 frames each, 12 bytes of PCM.
    const std::uint8_t data[10] = {0, 0, 0, 0, 0x07, 0, 0, 0, 0, 0x07};
    rig.miles.LoadSampleBuffer(rig.sample, 0, data, sizeof data);
    CHECK(rig.audio.log.Count("QueueBuffer") == 1);
    CHECK(rig.audio.log.Calls()[3] == "QueueBuffer(1, 12)");
}

TEST_CASE("AIL_set_sample_volume clamps, applies and reads back") {
    Rig rig;
    rig.Load(0);
    rig.audio.log.Clear();
    rig.miles.SetSampleVolume(rig.sample, 200);
    CHECK(rig.miles.SampleVolume(rig.sample) == 127);
    rig.miles.SetSampleVolume(rig.sample, -5);
    CHECK(rig.miles.SampleVolume(rig.sample) == 0);
    CHECK(rig.audio.log.Calls() == Calls{"SetStreamGain(1, 1)", "SetStreamPan(1, 0)",
                                         "SetStreamGain(1, 0)", "SetStreamPan(1, 0)"});
    CHECK(rig.miles.SampleVolume(42) == 0);
}

TEST_CASE("AIL_set_sample_pan maps 0 to 127 onto left to right") {
    CHECK(panFromMiles(0) == -1.0f);
    CHECK(panFromMiles(32) == -0.5f);
    CHECK(panFromMiles(64) == 0.0f);
    CHECK(panFromMiles(127) == 1.0f);
    CHECK(panFromMiles(500) == 1.0f);
    Rig rig;
    rig.Load(0);
    rig.audio.log.Clear();
    rig.miles.SetSamplePan(rig.sample, 0);
    CHECK(rig.audio.log.Calls() == Calls{"SetStreamGain(1, 1)", "SetStreamPan(1, -1)"});
}

TEST_CASE("AIL_set_digital_master_volume scales the whole output") {
    Rig rig;
    rig.miles.SetDigitalMasterVolume(254);
    rig.miles.SetDigitalMasterVolume(0);
    CHECK(rig.audio.log.Calls() == Calls{"SetMasterGain(1)", "SetMasterGain(0)"});
}

TEST_CASE("AIL_set_sample_user_data keeps eight slots") {
    Rig rig;
    rig.miles.SetSampleUserData(rig.sample, 3, 0xBEEF);
    rig.miles.SetSampleUserData(rig.sample, 7, -1);
    rig.miles.SetSampleUserData(rig.sample, 8, 5);
    CHECK(rig.miles.SampleUserData(rig.sample, 3) == 0xBEEF);
    CHECK(rig.miles.SampleUserData(rig.sample, 7) == -1);
    CHECK(rig.miles.SampleUserData(rig.sample, 8) == 0);
    CHECK(rig.miles.SampleUserData(99, 3) == 0);
}

TEST_CASE("AIL_sample_status reads free, done and playing") {
    Rig rig;
    CHECK(rig.miles.SampleStatus(0) == kSmpFree);
    CHECK(rig.miles.SampleStatus(rig.sample) == kSmpDone);
    rig.Load(0);
    rig.audio.queuedBuffersResult = std::size_t{1};
    CHECK(rig.miles.SampleStatus(rig.sample) == kSmpPlaying);
    rig.audio.queuedBuffersResult = std::size_t{0};
    CHECK(rig.miles.SampleStatus(rig.sample) == kSmpDone);
}

TEST_CASE("AIL_minimum_sample_buffer_size is about 50 ms in 2 KB units") {
    AudioMock audio;
    Miles miles{audio};
    CHECK(miles.MinimumSampleBufferSize(22050, kDigF16Bits) == 4096);
    CHECK(miles.MinimumSampleBufferSize(44100, kDigF16Bits | kDigFStereo) == 10240);
    CHECK(miles.MinimumSampleBufferSize(11025, 0) == 2048);
    CHECK(miles.MinimumSampleBufferSize(22050, kDigFAdpcm) == 2048);
    CHECK(miles.MinimumSampleBufferSize(0, kDigF16Bits) == 4096);
}

TEST_CASE("AIL_sample_buffer_ready alternates the buffers and refuses when both wait") {
    Rig rig;
    CHECK(rig.miles.SampleBufferReady(rig.sample) == 0);
    rig.Load(0);
    rig.audio.queuedBuffersResult = std::size_t{1};
    CHECK(rig.miles.SampleBufferReady(rig.sample) == 1);
    rig.Load(1);
    rig.audio.queuedBuffersResult = std::size_t{2};
    CHECK(rig.miles.SampleBufferReady(rig.sample) == -1);
    rig.audio.queuedBuffersResult = std::size_t{1};
    CHECK(rig.miles.SampleBufferReady(rig.sample) == 0);
    rig.miles.LoadSampleBuffer(rig.sample, 0, nullptr, 0); // the end
    CHECK(rig.miles.SampleBufferReady(rig.sample) == -1);
    CHECK(rig.miles.SampleBufferReady(99) == -1);
}

TEST_CASE("AIL_load_sample_buffer queues, starts once and ignores a third buffer") {
    Rig rig;
    rig.Load(0);
    rig.Load(1);
    rig.Load(2);
    CHECK(rig.audio.log.Count("QueueBuffer") == 2);
    CHECK(rig.audio.log.Count("PlayStream") == 1);
    rig.audio.log.Clear();
    rig.audio.failWith = Error::kOutOfResources;
    rig.Load(0); // a full queue: dropped
    CHECK(rig.audio.log.Count("QueueBuffer") == 1);
}

TEST_CASE("AIL_load_sample_buffer marks the end with zero bytes") {
    Rig rig;
    rig.Load(0);
    std::uint8_t byte = 0;
    rig.miles.LoadSampleBuffer(rig.sample, 1, &byte, 0);
    rig.audio.queuedBuffersResult = std::size_t{0};
    CHECK(rig.miles.SampleStatus(rig.sample) == kSmpDone);
    CHECK(rig.miles.SampleBufferReady(rig.sample) == -1);
    rig.Load(0); // more data clears the mark
    CHECK(rig.miles.SampleBufferReady(rig.sample) == 1);
}

TEST_CASE("a loaded buffer becomes 16-bit PCM whatever its format") {
    const std::uint8_t unsigned8[3] = {0x80, 0xFF, 0x00};
    CHECK(toPcm16(0, 0, 0, unsigned8, 3) ==
          std::vector<std::uint8_t>{0x00, 0x00, 0x00, 0x7F, 0x00, 0x80});
    const std::uint8_t signed8[2] = {0x01, 0x80};
    CHECK(toPcm16(0, kDigPcmSign, 0, signed8, 2) ==
          std::vector<std::uint8_t>{0x00, 0x01, 0x00, 0x80});
    const std::uint8_t pcm16[5] = {0x34, 0x12, 0x78, 0x56, 0x99}; // the odd byte is dropped
    CHECK(toPcm16(kDigF16Bits, 0, 0, pcm16, 5) ==
          std::vector<std::uint8_t>{0x34, 0x12, 0x78, 0x56});
    // Swapped stereo: right first in the data, put back as left then right.
    CHECK(toPcm16(kDigF16Bits | kDigFStereo, kDigPcmOrder, 0, pcm16, 4) ==
          std::vector<std::uint8_t>{0x78, 0x56, 0x34, 0x12});
    // ADPCM with no block size told: the whole load is one block.
    const std::uint8_t adpcm[5] = {0, 0, 0, 0, 0x07};
    CHECK(toPcm16(kDigFAdpcm, 0, 0, adpcm, 5) ==
          std::vector<std::uint8_t>{0, 0, 11, 0, 13, 0});
}

TEST_CASE("AIL_serve drains the completion queue") {
    Rig rig;
    platform::Completion done;
    done.kind = platform::CompletionKind::kStreamBufferDone;
    done.stream = StreamId{1};
    rig.audio.completions.push_back(done);
    rig.audio.completions.push_back(done);
    rig.miles.Serve();
    CHECK(rig.audio.completions.empty());
    CHECK(rig.audio.log.Count("PollCompletion") == 3);
}

TEST_CASE("AIL_shutdown releases every sample and closes the output") {
    Rig rig;
    rig.Load(0);
    rig.audio.log.Clear();
    rig.miles.Shutdown();
    CHECK(rig.audio.log.Calls() == Calls{"CloseStream(1)", "Close()"});
    CHECK(rig.miles.SampleStatus(rig.sample) == kSmpFree);
}

TEST_CASE("AIL_last_error is empty until a call fails") {
    AudioMock audio;
    Miles miles{audio};
    CHECK(miles.LastError().empty());
}

TEST_CASE("AIL_set_DirectSound_HWND needs no window") {
    AudioMock audio;
    Miles miles{audio};
    CHECK(miles.SetDirectSoundHwnd(1, 0x1234) == 0);
}

TEST_CASE("AIL_set_redist_directory returns the previous directory") {
    AudioMock audio;
    Miles miles{audio};
    CHECK(miles.SetRedistDirectory("redist").empty());
    CHECK(miles.SetRedistDirectory(nullptr) == "redist");
    CHECK(miles.SetRedistDirectory("other") == "redist");
    CHECK(miles.SetRedistDirectory("x") == "other");
}

} // namespace sfcop::hle::miles
