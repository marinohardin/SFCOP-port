// IDirectSound on the Audio component: one device, its primary buffer, its secondary buffers and
// its one 3D listener. DirectSoundCreate is Open(); the guest shims hold a COM object for each
// object here and call these methods. The device owns every buffer it creates; a buffer lives
// until ReleaseBuffer() (its COM reference count reaching zero) or the device's destruction.
//
// Not thread-safe: the guest shims call it from the game's threads under their own lock.
#pragma once

#include "hle/ds3d/DsTypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace sfcop::hle::ds3d {

class Listener;
class SoundBuffer;

// Where a secondary buffer's bytes live. The game writes them directly through the pointers
// Lock returns, so the guest shims supply memory the guest can address; tests use the host's.
class Memory {
public:
    virtual ~Memory() = default;
    // Zero-filled memory of `bytes`, or nullptr when none is left.
    virtual std::uint8_t *Allocate(std::size_t bytes) = 0;
    virtual void Free(std::uint8_t *memory) = 0;
};

// Memory from the host's heap.
Memory &hostMemory();

class DirectSound {
public:
    DirectSound(platform::Audio &audio, Memory &memory);
    ~DirectSound();
    DirectSound(const DirectSound &) = delete;
    DirectSound &operator=(const DirectSound &) = delete;

    // DirectSoundCreate: opens the output, 44100 Hz stereo; kDsErrNoDriver when it does not
    // open. An output already open is used as it is.
    HResult Open();

    // A window is required (kDsErrInvalidParam without one) and the level is one of
    // DSSCL_NORMAL, DSSCL_PRIORITY, DSSCL_EXCLUSIVE (3) or DSSCL_WRITEPRIMARY.
    HResult SetCooperativeLevel(bool hasWindow, std::uint32_t level);
    std::uint32_t CooperativeLevel() const { return level_; }

    // A device that plays 8- and 16-bit, mono and stereo buffers at 100 to 100000 Hz and mixes
    // 64 of them; the host mixes every one.
    Caps GetCaps() const;

    // The primary buffer (DSBCAPS_PRIMARYBUFFER, no size and no format) is created once and
    // returned again while it lives; with DSBCAPS_CTRL3D it carries the listener. A secondary
    // buffer needs a PCM format (8 or 16 bits, 1 or 2 channels, 100 to 100000 Hz, a block
    // alignment of channels times bytes per sample) and a size of 4 to 0x0FFFFFFF bytes, and
    // starts silent; DSBCAPS_CTRL3D needs mono. Errors: kDsErrInvalidParam for a bad
    // description, kDsErrBadFormat for a bad format, kDsErrOutOfMemory, kDsErrInvalidCall
    // before Open().
    HResult CreateSoundBuffer(const BufferDesc &desc, SoundBuffer **out);

    // A secondary buffer sharing `source`'s bytes, with its flags, format, volume, pan and
    // frequency, stopped at the start, and new 3D parameters. The primary buffer cannot be
    // duplicated (kDsErrInvalidCall).
    HResult DuplicateSoundBuffer(SoundBuffer &source, SoundBuffer **out);

    // Destroys a buffer the device created. Releasing the primary buffer keeps the listener.
    void ReleaseBuffer(SoundBuffer *buffer);

    // DSSPEAKER_STEREO until set; any value is kept.
    std::uint32_t GetSpeakerConfig() const { return speakerConfig_; }
    HResult SetSpeakerConfig(std::uint32_t config);

    HResult Compact() const { return kDsOk; }
    // A device made by DirectSoundCreate is initialized already.
    HResult Initialize() const { return kDsErrAlreadyInitialized; }

    // The listener, made with the first primary buffer that has DSBCAPS_CTRL3D; nullptr before.
    Listener *GetListener() { return listener_.get(); }

    // CommitDeferredSettings: the listener's and every 3D buffer's deferred changes applied.
    void CommitDeferred();

    // Discards the Audio completion queue; a buffer's status is read from its voice.
    void DrainCompletions();

    platform::Audio &AudioOut() { return audio_; }
    Memory &BufferMemory() { return memory_; }

private:
    SoundBuffer *add(std::unique_ptr<SoundBuffer> buffer);

    platform::Audio &audio_;
    Memory &memory_;
    bool open_ = false;
    std::uint32_t level_ = 0;
    std::uint32_t speakerConfig_ = kDsSpeakerStereo;
    std::vector<std::unique_ptr<SoundBuffer>> buffers_;
    SoundBuffer *primary_ = nullptr;
    std::unique_ptr<Listener> listener_;
};

// The checks CreateSoundBuffer makes on a secondary buffer's format.
bool validFormat(const WaveFormat &format);

} // namespace sfcop::hle::ds3d
