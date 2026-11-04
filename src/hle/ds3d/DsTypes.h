// What the DirectSound module shares: the result codes, flags and structures of the public
// DirectSound and DirectSound3D interfaces as values (the guest shims read and write the guest's
// structures field by field), and the conversions from DirectSound's scales to the Audio
// component's.
#pragma once

#include "platform/Audio/Audio.h"

#include <cstdint>

namespace sfcop::hle::ds3d {

using HResult = std::uint32_t;

constexpr HResult kDsOk = 0x00000000;
constexpr HResult kDsNoVirtualization = 0x0878000A; // a success code; not returned here
constexpr HResult kENoInterface = 0x80004002;
constexpr HResult kDsErrInvalidParam = 0x80070057;
constexpr HResult kDsErrOutOfMemory = 0x8007000E;
constexpr HResult kDsErrControlUnavail = 0x8878001E;
constexpr HResult kDsErrInvalidCall = 0x88780032;
constexpr HResult kDsErrPrioLevelNeeded = 0x88780046;
constexpr HResult kDsErrBadFormat = 0x88780064;
constexpr HResult kDsErrNoDriver = 0x88780078;
constexpr HResult kDsErrAlreadyInitialized = 0x88780082;

// DSBCAPS_*
constexpr std::uint32_t kDsbCapsPrimaryBuffer = 0x00000001;
constexpr std::uint32_t kDsbCapsStatic = 0x00000002;
constexpr std::uint32_t kDsbCapsLocHardware = 0x00000004;
constexpr std::uint32_t kDsbCapsLocSoftware = 0x00000008;
constexpr std::uint32_t kDsbCapsCtrl3D = 0x00000010;
constexpr std::uint32_t kDsbCapsCtrlFrequency = 0x00000020;
constexpr std::uint32_t kDsbCapsCtrlPan = 0x00000040;
constexpr std::uint32_t kDsbCapsCtrlVolume = 0x00000080;
// DSBPLAY_*, DSBSTATUS_*, DSBLOCK_*
constexpr std::uint32_t kDsbPlayLooping = 0x00000001;
constexpr std::uint32_t kDsbStatusPlaying = 0x00000001;
constexpr std::uint32_t kDsbStatusLooping = 0x00000004;
constexpr std::uint32_t kDsbLockFromWriteCursor = 0x00000001;
constexpr std::uint32_t kDsbLockEntireBuffer = 0x00000002;
// DSSCL_*
constexpr std::uint32_t kDsSclNormal = 1;
constexpr std::uint32_t kDsSclPriority = 2;
constexpr std::uint32_t kDsSclExclusive = 3;
constexpr std::uint32_t kDsSclWritePrimary = 4;
// DSSPEAKER_STEREO
constexpr std::uint32_t kDsSpeakerStereo = 4;
// DS3DMODE_* and DS3D_* apply flags
constexpr std::uint32_t kDs3dModeNormal = 0;
constexpr std::uint32_t kDs3dModeHeadRelative = 1;
constexpr std::uint32_t kDs3dModeDisable = 2;
constexpr std::uint32_t kDs3dImmediate = 0;
constexpr std::uint32_t kDs3dDeferred = 1;

constexpr std::int32_t kDsbVolumeMin = -10000; // hundredths of a decibel
constexpr std::int32_t kDsbPanLeft = -10000;
constexpr std::int32_t kDsbPanRight = 10000;
constexpr std::uint32_t kDsbFrequencyOriginal = 0;
constexpr std::uint32_t kDsbFrequencyMin = 100;
constexpr std::uint32_t kDsbFrequencyMax = 100000;
constexpr std::uint32_t kDsbSizeMin = 4;
constexpr std::uint32_t kDsbSizeMax = 0x0FFFFFFF;
constexpr std::uint16_t kWaveFormatPcm = 1;

// The output the device opens.
constexpr std::uint32_t kOutputRate = 44100;

struct WaveFormat { // WAVEFORMATEX without cbSize, which is 0 for PCM
    std::uint16_t formatTag = kWaveFormatPcm;
    std::uint16_t channels = 2;
    std::uint32_t samplesPerSec = 22050;
    std::uint32_t avgBytesPerSec = 22050 * 2;
    std::uint16_t blockAlign = 2;
    std::uint16_t bitsPerSample = 8;
};

struct BufferDesc { // DSBUFFERDESC
    std::uint32_t flags = 0;
    std::uint32_t bufferBytes = 0;
    bool hasFormat = false;
    WaveFormat format;
};

struct Caps { // DSCAPS after dwSize, in order
    std::uint32_t flags = 0;
    std::uint32_t minSecondarySampleRate = 0;
    std::uint32_t maxSecondarySampleRate = 0;
    std::uint32_t primaryBuffers = 0;
    std::uint32_t maxHwMixingAllBuffers = 0;
    std::uint32_t maxHwMixingStaticBuffers = 0;
    std::uint32_t maxHwMixingStreamingBuffers = 0;
    std::uint32_t freeHwMixingAllBuffers = 0;
    std::uint32_t freeHwMixingStaticBuffers = 0;
    std::uint32_t freeHwMixingStreamingBuffers = 0;
    // The 3D hardware counts, memory, transfer rate, overhead and reserved fields are 0.
};

struct BufferCaps { // DSBCAPS after dwSize
    std::uint32_t flags = 0;
    std::uint32_t bufferBytes = 0;
    std::uint32_t unlockTransferRate = 0;
    std::uint32_t playCpuOverhead = 0;
};

struct Vector { // D3DVECTOR
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Buffer3DParams { // DS3DBUFFER after dwSize
    Vector position;
    Vector velocity;
    std::uint32_t insideConeAngle = 360;
    std::uint32_t outsideConeAngle = 360;
    Vector coneOrientation{0.0f, 0.0f, 1.0f};
    std::int32_t coneOutsideVolume = 0;
    float minDistance = 1.0f;
    float maxDistance = 1000000000.0f;
    std::uint32_t mode = kDs3dModeNormal;
};

struct ListenerParams { // DS3DLISTENER after dwSize
    Vector position;
    Vector velocity;
    Vector front{0.0f, 0.0f, 1.0f};
    Vector top{0.0f, 1.0f, 0.0f};
    float distanceFactor = 1.0f;
    float rolloffFactor = 1.0f;
    float dopplerFactor = 1.0f;
};

// Hundredths of a decibel (-10000 to 0) as a linear gain; -10000 is silence.
float gainFromVolume(std::int32_t volume);
// DirectSound's pan (-10000 to 10000: the other side attenuated by that many hundredths of a
// decibel) as the Audio component's pan, which has the same law on a scale of -1 to +1.
float panFromDs(std::int32_t pan);
// Valid DirectSound3D buffer parameters (the ranges the methods check) as the Audio component's.
platform::Voice3D toVoice3D(const Buffer3DParams &params);
platform::Listener toListener(const ListenerParams &params);

// The checks the Set methods make.
bool validBuffer3D(const Buffer3DParams &params);
bool validListener(const ListenerParams &params);

} // namespace sfcop::hle::ds3d
