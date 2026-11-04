// Audio: the mixer behind the Miles sample API and the DirectSound and DirectSound3D interfaces
// the game uses (DESIGN §4.7, §5.8, §6.2). Clips hold sound data; a voice plays one clip, with
// its own gain, pan, rate, loop and position, and optionally 3D parameters relative to the one
// listener; a stream plays buffers queued one after another (music, and the Miles double-buffered
// samples). Units are neutral: linear gain, pan from -1 to +1, rates in Hz, angles in degrees.
// The HLE modules convert the game's hundredths of a decibel and 0-127 scales.
//
// 3D space is left-handed as the game's is: +x right, +y up, +z forward, distances in the game's
// units scaled to metres by Listener::distanceFactor.
//
// Threading: every method may be called from any thread. Mixing runs on the backend's own thread
// (DESIGN §7.3); a change is heard from the next mix period (at most about 20 ms later). Nothing
// calls back into the caller: the mixer records a voice reaching its end, or a stream finishing a
// buffer, in a completion queue, and the caller drains it with PollCompletion() on its own thread
// (the Miles HLE does so from its service tick and runs the game's end-of-sample handling there).
#pragma once

#include "platform/Core/Handle.h"
#include "platform/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace sfcop::platform {

struct ClipTag {};
using ClipId = Handle<ClipTag>;
struct VoiceTag {};
using VoiceId = Handle<VoiceTag>;
struct StreamTag {};
using StreamId = Handle<StreamTag>;

enum class SampleFormat : std::uint8_t {
    kPcm8,     // unsigned 8-bit, 128 = silence
    kPcm16,    // signed 16-bit little-endian
    kImaAdpcm, // IMA ADPCM, 4 bits per sample, in blocks of `adpcmBlockBytes` (the WAV layout)
};

struct ClipFormat {
    SampleFormat format = SampleFormat::kPcm16;
    std::uint8_t channels = 1;        // 1 or 2; stereo is interleaved
    std::uint32_t sampleRate = 22050; // Hz, 1000 to 192000
    std::uint32_t adpcmBlockBytes = 0; // kImaAdpcm only: bytes per block (per channel set)
};

struct OutputDesc {
    std::uint32_t sampleRate = 44100;
    std::uint8_t channels = 2;
};

struct Vector3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

enum class VoiceSpace : std::uint8_t {
    k2d,           // gain and pan only; the 3D parameters are kept but not applied
    kWorld,        // positioned in the world, relative to the listener
    kHeadRelative, // position and velocity are relative to the listener already
};

// A voice's 3D parameters (DirectSound3D buffer).
struct Voice3D {
    VoiceSpace space = VoiceSpace::k2d;
    Vector3 position;
    Vector3 velocity; // units per second
    float minDistance = 1.0f;          // full gain at or inside this distance
    float maxDistance = 1000000000.0f; // no further attenuation beyond this distance
    std::uint32_t coneInsideDegrees = 360;  // 0 to 360; full gain inside
    std::uint32_t coneOutsideDegrees = 360; // inside to 360; coneOutsideGain outside
    Vector3 coneOrientation{0.0f, 0.0f, 1.0f};
    float coneOutsideGain = 1.0f; // 0 to 1
};

// The listener (DirectSound3D listener).
struct Listener {
    Vector3 position;
    Vector3 velocity;
    Vector3 forward{0.0f, 0.0f, 1.0f};
    Vector3 up{0.0f, 1.0f, 0.0f};
    float distanceFactor = 1.0f; // metres per game unit
    float rolloffFactor = 1.0f;  // 0 = no distance attenuation, 1 = real world, up to 10
    float dopplerFactor = 1.0f;  // 0 = none, 1 = real world, up to 10
};

enum class VoiceState : std::uint8_t {
    kStopped, // never played, stopped, or reached the end without looping
    kPlaying,
};

struct VoiceStatus {
    VoiceState state = VoiceState::kStopped;
    std::uint64_t frame = 0; // play position in sample frames from the clip's start
};

enum class CompletionKind : std::uint8_t {
    kVoiceEnded,       // `voice` played to its clip's end without looping and stopped
    kStreamBufferDone, // `stream` finished playing one queued buffer
};

// One entry of the completion queue.
struct Completion {
    CompletionKind kind = CompletionKind::kVoiceEnded;
    VoiceId voice;   // kVoiceEnded
    StreamId stream; // kStreamBufferDone
};

class Audio {
public:
    virtual ~Audio() = default;

    // Opens the output device. Errors: kUnsupported; kWrongState if open; kInvalidArgument for
    // a rate outside 8000-192000 or channels other than 1 or 2; kIoError if no device opens.
    virtual Status Open(const OutputDesc &desc) = 0;

    // Stops every voice and stream, frees every clip, and closes the device. Closing a closed
    // device succeeds. Errors: kUnsupported.
    virtual Status Close() = 0;

    // Copies `bytes` of sound in `format` into a new clip. ADPCM may be decoded now; the clip
    // keeps its frame count either way. Errors: kUnsupported; kWrongState if not open;
    // kInvalidArgument for a bad format, an empty buffer, or a size that is not whole frames
    // (whole blocks for ADPCM); kOutOfResources.
    virtual Result<ClipId> LoadClip(const ClipFormat &format, const void *data,
                                    std::size_t bytes) = 0;

    // Overwrites part of a PCM clip with new bytes in its own format, as a game writes into a
    // sound buffer that may be playing; voices hear the new data from the next mix. Errors:
    // kUnsupported; kInvalidHandle; kInvalidArgument for an ADPCM clip or a range past the end.
    virtual Status WriteClip(ClipId clip, std::size_t offsetBytes, const void *data,
                             std::size_t bytes) = 0;

    // Frees a clip; voices using it stop and are destroyed. Errors: kUnsupported;
    // kInvalidHandle.
    virtual Status ReleaseClip(ClipId clip) = 0;

    // Creates a stopped voice on `clip` at frame 0, gain 1, pan 0, the clip's rate, 2D.
    // Errors: kUnsupported; kInvalidHandle; kOutOfResources when the voice limit (at least 64)
    // is reached.
    virtual Result<VoiceId> CreateVoice(ClipId clip) = 0;

    // Frees a voice, stopping it. Errors: kUnsupported; kInvalidHandle.
    virtual Status DestroyVoice(VoiceId voice) = 0;

    // Plays from the current position. With `loop`, the clip repeats from frame 0 at its end;
    // without, the voice stops at the end with its position at the end. Playing a playing voice
    // only changes `loop`. Errors: kUnsupported; kInvalidHandle.
    virtual Status Play(VoiceId voice, bool loop) = 0;

    // Stops a voice, keeping its position. Errors: kUnsupported; kInvalidHandle.
    virtual Status Stop(VoiceId voice) = 0;

    // Moves the play position. Errors: kUnsupported; kInvalidHandle; kInvalidArgument past the
    // clip's end.
    virtual Status SetPosition(VoiceId voice, std::uint64_t frame) = 0;

    // Playing or stopped, and the position. Errors: kUnsupported; kInvalidHandle.
    virtual Result<VoiceStatus> GetStatus(VoiceId voice) = 0;

    // Linear gain, 0 (silent) to 1 (as recorded). Errors: kUnsupported; kInvalidHandle;
    // kInvalidArgument outside 0-1.
    virtual Status SetGain(VoiceId voice, float gain) = 0;

    // Stereo position of a 2D voice: -1 left, 0 centre, +1 right, DirectSound's law (the near
    // side at unity, the far side attenuated by |pan| x 100 dB; both at unity at the centre). Errors:
    // kUnsupported; kInvalidHandle; kInvalidArgument outside -1 to +1.
    virtual Status SetPan(VoiceId voice, float pan) = 0;

    // Playback rate in Hz (the clip's rate plays at normal speed and pitch). Errors:
    // kUnsupported; kInvalidHandle; kInvalidArgument outside 100-192000.
    virtual Status SetRate(VoiceId voice, std::uint32_t hz) = 0;

    // Sets a voice's 3D parameters, all at once. Errors: kUnsupported; kInvalidHandle;
    // kInvalidArgument for a negative or inverted distance range, angles out of order or over
    // 360, or a gain outside 0-1.
    virtual Status SetVoice3D(VoiceId voice, const Voice3D &params) = 0;

    // Sets the listener, all at once. Errors: kUnsupported; kInvalidArgument for a zero
    // forward or up vector, or a factor out of range.
    virtual Status SetListener(const Listener &listener) = 0;

    // Opens a stream whose buffers are all in `format`. It is stopped and empty. Errors:
    // kUnsupported; kWrongState if not open; kInvalidArgument as LoadClip(); kOutOfResources.
    virtual Result<StreamId> OpenStream(const ClipFormat &format) = 0;

    // Copies a buffer to the end of the stream's queue. A playing stream that runs out of
    // buffers plays silence and stays playing. Errors: kUnsupported; kInvalidHandle;
    // kInvalidArgument as LoadClip(); kOutOfResources when 16 buffers are queued.
    virtual Status QueueBuffer(StreamId stream, const void *data, std::size_t bytes) = 0;

    // How many queued buffers have not finished playing (the one playing counts). Errors:
    // kUnsupported; kInvalidHandle.
    virtual Result<std::size_t> QueuedBuffers(StreamId stream) = 0;

    // Starts or resumes a stream. Errors: kUnsupported; kInvalidHandle.
    virtual Status PlayStream(StreamId stream) = 0;

    // Pauses a stream; the queue is kept. Errors: kUnsupported; kInvalidHandle.
    virtual Status StopStream(StreamId stream) = 0;

    // Discards every queued buffer; the stream stays playing or stopped. Errors: kUnsupported;
    // kInvalidHandle.
    virtual Status FlushStream(StreamId stream) = 0;

    // Linear gain of a stream, as SetGain(). Errors: kUnsupported; kInvalidHandle;
    // kInvalidArgument outside 0-1.
    virtual Status SetStreamGain(StreamId stream, float gain) = 0;

    // Stereo position of a stream, as SetPan(). Errors: kUnsupported; kInvalidHandle;
    // kInvalidArgument outside -1 to +1.
    virtual Status SetStreamPan(StreamId stream, float pan) = 0;

    // Stops and frees a stream. Errors: kUnsupported; kInvalidHandle.
    virtual Status CloseStream(StreamId stream) = 0;

    // Gain applied to everything, 0 to 1. Errors: kUnsupported; kInvalidArgument outside 0-1.
    virtual Status SetMasterGain(float gain) = 0;

    // Removes and returns the oldest completion, or std::nullopt if none is queued. The mixer
    // thread queues one when a voice reaches its clip's end without looping (once per play) and
    // when a stream finishes a buffer; Stop(), StopStream() and FlushStream() queue nothing.
    // Completions of a voice or stream that is then destroyed or closed are removed. The queue
    // holds at least 1024; beyond that the newest are dropped, so drain it every service tick.
    // Any thread may drain it; with several draining, each completion goes to one of them.
    // Errors: kUnsupported.
    virtual Result<std::optional<Completion>> PollCompletion() = 0;
};

// An Audio whose every method fails with kUnsupported.
std::unique_ptr<Audio> makeAudioStub();

// The build's real backend, on miniaudio (B4); declared here so that the client needs no
// backend header, defined by the backend's own source file once it lands.
std::unique_ptr<Audio> makeAudio();

} // namespace sfcop::platform
