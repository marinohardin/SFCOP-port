// The guest shims of the Miles module: the MSS32.DLL rows of src/hle/surface.tsv, in pcrecomp's
// win32hle form, by their decorated import names. Each reads its arguments from the guest stack,
// calls the module under one lock, and purges the stack the name's @N gives.
//
// The game keeps a digital driver handle and sample handles. Each is a small block of guest
// memory, kept for the run, holding a mark and the module's handle, so a handle from another
// source or a released one is ignored as the module ignores it.
extern "C" {
#include "win32hle.h"
}

#include "hle/miles/Miles.h"
#include "platform/Audio/AudioMiniaudio.h"

#include <array>
#include <memory>
#include <mutex>
#include <string>

// The boot host's --silent and --headless flag: no sound reaches a device while it is set.
extern "C" int sfcop_mss_silent;

namespace sfcop::hle::miles {
namespace {

constexpr std::uint32_t kSampleMark = 0x4C504D53; // "SMPL"
constexpr std::uint32_t kDriverMark = 0x56524944; // "DIRV"
constexpr std::uint32_t kHandleBytes = 0x40;
constexpr std::uint32_t kDriverBytes = 0x100;
constexpr std::uint32_t kWaveOutOffset = 0x80; // the HWAVEOUT handed out, inside the driver block
constexpr std::uint32_t kPathBytes = 260;

std::mutex g_lock;
std::unique_ptr<platform::AudioMiniaudio> g_audio;
std::unique_ptr<Miles> g_miles;
std::uint32_t g_driver;
std::array<std::uint32_t, kMaxSamples + 1> g_handles{}; // by module handle; 0 unused
std::uint32_t g_lastError;
std::string g_lastErrorText;

Miles &miles() {
    if (!g_miles) {
        g_audio = std::make_unique<platform::AudioMiniaudio>(
            sfcop_mss_silent ? platform::AudioOutput::kNullDevice
                             : platform::AudioOutput::kDevice);
        g_miles = std::make_unique<Miles>(*g_audio);
    }
    return *g_miles;
}

// The module's handle for a guest sample handle, or 0.
std::uint32_t sampleOf(std::uint32_t handle) {
    if (handle == 0 || MEM32(handle) != kSampleMark) {
        return 0;
    }
    return MEM32(handle + 4);
}

std::uint32_t guestSample(std::uint32_t sample) {
    if (g_handles[sample] == 0) {
        g_handles[sample] = hle_va(hle_alloc(kHandleBytes));
    }
    std::uint32_t handle = g_handles[sample];
    MEM32(handle) = kSampleMark;
    MEM32(handle + 4) = sample;
    return handle;
}

void a_startup() {
    std::lock_guard<std::mutex> guard(g_lock);
    RET(miles().Startup(), 0);
}
void a_shutdown() {
    std::lock_guard<std::mutex> guard(g_lock);
    miles().Shutdown();
    RETV(0);
}
void a_waveOutOpen() { // (&driver, &waveout, device, format)
    std::lock_guard<std::mutex> guard(g_lock);
    std::int32_t failed = miles().WaveOutOpen();
    if (failed == 0) {
        if (g_driver == 0) {
            g_driver = hle_va(hle_alloc(kDriverBytes));
            MEM32(g_driver) = kDriverMark;
        }
        if (A32(0) != 0) {
            MEM32(A32(0)) = g_driver;
        }
        if (A32(1) != 0) {
            MEM32(A32(1)) = g_driver + kWaveOutOffset;
        }
    }
    RET(failed, 4);
}
void a_waveOutClose() { // (driver)
    std::lock_guard<std::mutex> guard(g_lock);
    miles().WaveOutClose();
    RETV(1);
}
void a_allocate_sample_handle() { // (driver)
    std::lock_guard<std::mutex> guard(g_lock);
    std::uint32_t sample = miles().AllocateSampleHandle();
    RET(sample != 0 ? guestSample(sample) : 0, 1);
}
void a_release_sample_handle() { // (sample)
    std::lock_guard<std::mutex> guard(g_lock);
    std::uint32_t sample = sampleOf(A32(0));
    if (sample != 0) {
        miles().ReleaseSampleHandle(sample);
        MEM32(A32(0)) = 0;
    }
    RETV(1);
}
void a_init_sample() { // (sample)
    std::lock_guard<std::mutex> guard(g_lock);
    miles().InitSample(sampleOf(A32(0)));
    RETV(1);
}
void a_end_sample() { // (sample)
    std::lock_guard<std::mutex> guard(g_lock);
    miles().EndSample(sampleOf(A32(0)));
    RETV(1);
}
void a_set_sample_type() { // (sample, format, flags)
    std::lock_guard<std::mutex> guard(g_lock);
    miles().SetSampleType(sampleOf(A32(0)), static_cast<std::int32_t>(A32(1)), A32(2));
    RETV(3);
}
void a_set_sample_playback_rate() { // (sample, hz)
    std::lock_guard<std::mutex> guard(g_lock);
    miles().SetSamplePlaybackRate(sampleOf(A32(0)), static_cast<std::int32_t>(A32(1)));
    RETV(2);
}
void a_set_sample_adpcm_block_size() { // (sample, bytes)
    std::lock_guard<std::mutex> guard(g_lock);
    miles().SetSampleAdpcmBlockSize(sampleOf(A32(0)), A32(1));
    RETV(2);
}
void a_set_sample_volume() { // (sample, volume)
    std::lock_guard<std::mutex> guard(g_lock);
    miles().SetSampleVolume(sampleOf(A32(0)), static_cast<std::int32_t>(A32(1)));
    RETV(2);
}
void a_sample_volume() { // (sample)
    std::lock_guard<std::mutex> guard(g_lock);
    RET(miles().SampleVolume(sampleOf(A32(0))), 1);
}
void a_set_sample_pan() { // (sample, pan)
    std::lock_guard<std::mutex> guard(g_lock);
    miles().SetSamplePan(sampleOf(A32(0)), static_cast<std::int32_t>(A32(1)));
    RETV(2);
}
void a_set_digital_master_volume() { // (driver, volume)
    std::lock_guard<std::mutex> guard(g_lock);
    miles().SetDigitalMasterVolume(static_cast<std::int32_t>(A32(1)));
    RETV(2);
}
void a_set_sample_user_data() { // (sample, index, value)
    std::lock_guard<std::mutex> guard(g_lock);
    miles().SetSampleUserData(sampleOf(A32(0)), A32(1), static_cast<std::int32_t>(A32(2)));
    RETV(3);
}
void a_sample_status() { // (sample)
    std::lock_guard<std::mutex> guard(g_lock);
    RET(miles().SampleStatus(sampleOf(A32(0))), 1);
}
void a_minimum_sample_buffer_size() { // (driver, hz, format)
    std::lock_guard<std::mutex> guard(g_lock);
    RET(miles().MinimumSampleBufferSize(static_cast<std::int32_t>(A32(1)),
                                        static_cast<std::int32_t>(A32(2))),
        3);
}
void a_sample_buffer_ready() { // (sample)
    std::lock_guard<std::mutex> guard(g_lock);
    RET(miles().SampleBufferReady(sampleOf(A32(0))), 1);
}
void a_load_sample_buffer() { // (sample, buffer, data, bytes)
    std::lock_guard<std::mutex> guard(g_lock);
    const auto *data = static_cast<const std::uint8_t *>(hle_ptr(A32(2)));
    miles().LoadSampleBuffer(sampleOf(A32(0)), A32(1), data, data != nullptr ? A32(3) : 0);
    RETV(4);
}
void a_serve() {
    std::lock_guard<std::mutex> guard(g_lock);
    miles().Serve();
    RETV(0);
}
void a_last_error() { // -> a guest string, kept until the error changes
    std::lock_guard<std::mutex> guard(g_lock);
    const std::string &text = miles().LastError();
    if (g_lastError == 0 || text != g_lastErrorText) {
        g_lastErrorText = text;
        g_lastError = hle_guest_strdup(text.c_str());
    }
    RET(g_lastError, 0);
}
void a_set_DirectSound_HWND() { // (driver, window)
    std::lock_guard<std::mutex> guard(g_lock);
    RET(miles().SetDirectSoundHwnd(A32(0), A32(1)), 2);
}
void a_set_redist_directory() { // (directory) -> the previous one, as a guest string
    std::lock_guard<std::mutex> guard(g_lock);
    std::string previous = miles().SetRedistDirectory(A32(0) != 0 ? ASTR(0) : nullptr);
    if (previous.size() >= kPathBytes) {
        previous.resize(kPathBytes - 1);
    }
    RET(hle_guest_strdup(previous.c_str()), 1);
}
void a_mem_alloc_lock() { // (bytes) -> guest memory
    RET(hle_va(hle_alloc(A32(0))), 1);
}
void a_mem_free_lock() { // (memory)
    if (A32(0) != 0) {
        hle_free(hle_ptr(A32(0)));
    }
    RETV(1);
}

} // namespace
} // namespace sfcop::hle::miles

using namespace sfcop::hle::miles;

extern "C" const win32hle_shim sfcop_hle_miles[] = {
    {"_AIL_allocate_sample_handle@4", a_allocate_sample_handle},
    {"_AIL_end_sample@4", a_end_sample},
    {"_AIL_init_sample@4", a_init_sample},
    {"_AIL_last_error@0", a_last_error},
    {"_AIL_load_sample_buffer@16", a_load_sample_buffer},
    {"_AIL_mem_alloc_lock@4", a_mem_alloc_lock},
    {"_AIL_mem_free_lock@4", a_mem_free_lock},
    {"_AIL_minimum_sample_buffer_size@12", a_minimum_sample_buffer_size},
    {"_AIL_release_sample_handle@4", a_release_sample_handle},
    {"_AIL_sample_buffer_ready@4", a_sample_buffer_ready},
    {"_AIL_sample_status@4", a_sample_status},
    {"_AIL_sample_volume@4", a_sample_volume},
    {"_AIL_serve@0", a_serve},
    {"_AIL_set_digital_master_volume@8", a_set_digital_master_volume},
    {"_AIL_set_DirectSound_HWND@8", a_set_DirectSound_HWND},
    {"_AIL_set_redist_directory@4", a_set_redist_directory},
    {"_AIL_set_sample_adpcm_block_size@8", a_set_sample_adpcm_block_size},
    {"_AIL_set_sample_pan@8", a_set_sample_pan},
    {"_AIL_set_sample_playback_rate@8", a_set_sample_playback_rate},
    {"_AIL_set_sample_type@12", a_set_sample_type},
    {"_AIL_set_sample_user_data@12", a_set_sample_user_data},
    {"_AIL_set_sample_volume@8", a_set_sample_volume},
    {"_AIL_shutdown@0", a_shutdown},
    {"_AIL_startup@0", a_startup},
    {"_AIL_waveOutClose@4", a_waveOutClose},
    {"_AIL_waveOutOpen@16", a_waveOutOpen},
    {nullptr, nullptr}};
