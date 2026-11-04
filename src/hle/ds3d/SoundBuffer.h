// IDirectSoundBuffer on the Audio component. A secondary buffer's bytes are one Audio clip,
// played by the buffer's own voice; the game writes them through Lock and Unlock, which copies
// the written ranges into the clip. Duplicates share the bytes and the clip, each with its own
// voice. The primary buffer is the output itself: its volume is the master gain, and it has no
// bytes the game can lock.
//
// A method that needs a control the buffer was not created with (DSBCAPS_CTRLVOLUME, _CTRLPAN,
// _CTRLFREQUENCY) returns kDsErrControlUnavail.
#pragma once

#include "hle/ds3d/DsTypes.h"

#include <cstdint>
#include <memory>

namespace sfcop::hle::ds3d {

class Buffer3D;
class DirectSound;
class Memory;

// The two pieces of a locked range: `first` at the offset asked for, `second` from the start of
// the buffer when the range wraps (nullptr and 0 when it does not).
struct LockedRange {
    std::uint8_t *first = nullptr;
    std::uint32_t firstBytes = 0;
    std::uint8_t *second = nullptr;
    std::uint32_t secondBytes = 0;
};

class SoundBuffer {
public:
    // The primary buffer.
    SoundBuffer(DirectSound &device, std::uint32_t flags);
    // A secondary buffer; Init() makes its bytes and voice.
    SoundBuffer(DirectSound &device, const BufferDesc &desc);
    ~SoundBuffer();
    SoundBuffer(const SoundBuffer &) = delete;
    SoundBuffer &operator=(const SoundBuffer &) = delete;

    // Silent bytes and a stopped voice, or shares `source`'s bytes. kDsErrOutOfMemory when the
    // memory, the clip or the voice cannot be had.
    HResult Init(const SoundBuffer *source = nullptr);

    bool IsPrimary() const { return primary_; }
    std::uint32_t Flags() const { return flags_; }

    // DSBCAPS: the flags (DSBCAPS_LOCSOFTWARE added) and the size.
    BufferCaps GetCaps() const;
    WaveFormat GetFormat() const { return format_; }
    // The primary buffer's format may be set at DSSCL_PRIORITY or above (kDsErrPrioLevelNeeded
    // below); the output keeps its own rate. A secondary buffer's cannot (kDsErrInvalidCall).
    HResult SetFormat(const WaveFormat &format);

    // Byte offsets: the play cursor, and the write cursor 10 ms ahead of it while playing (the
    // same as the play cursor when stopped). A buffer played to its end reads 0.
    HResult GetCurrentPosition(std::uint32_t *play, std::uint32_t *write);
    // kDsErrInvalidParam past the end; the offset is rounded down to whole frames.
    HResult SetCurrentPosition(std::uint32_t offset);

    // DSBSTATUS_PLAYING, with DSBSTATUS_LOOPING when played with DSBPLAY_LOOPING.
    HResult GetStatus(std::uint32_t *status);
    // Plays from the play cursor, or from the start when the buffer was played to its end.
    HResult Play(std::uint32_t flags);
    HResult Stop();

    // DSBLOCK_FROMWRITECURSOR locks from the write cursor; DSBLOCK_ENTIREBUFFER locks it all.
    // kDsErrInvalidParam for an offset past the end or a size of 0 or over the buffer's;
    // kDsErrPrioLevelNeeded for the primary buffer.
    HResult Lock(std::uint32_t offset, std::uint32_t bytes, std::uint32_t flags,
                 LockedRange *range);
    // Copies the written bytes of a range Lock returned into the clip; a piece outside the
    // buffer is kDsErrInvalidParam.
    HResult Unlock(const LockedRange &range);

    // Hundredths of a decibel, -10000 to 0.
    HResult GetVolume(std::int32_t *volume) const;
    HResult SetVolume(std::int32_t volume);
    // -10000 (left) to 10000 (right).
    HResult GetPan(std::int32_t *pan) const;
    HResult SetPan(std::int32_t pan);
    // Hz, 100 to 100000; DSBFREQUENCY_ORIGINAL (0) restores the format's rate.
    HResult GetFrequency(std::uint32_t *hz) const;
    HResult SetFrequency(std::uint32_t hz);

    // The bytes are never lost: kDsOk.
    HResult Restore() const { return kDsOk; }
    // A buffer made by CreateSoundBuffer is initialized already.
    HResult Initialize() const { return kDsErrAlreadyInitialized; }

    // The 3D buffer of a secondary buffer created with DSBCAPS_CTRL3D, else nullptr.
    Buffer3D *Get3D() { return buffer3D_.get(); }

private:
    struct Storage;

    std::uint32_t frames() const;
    std::uint32_t leadBytes() const;

    DirectSound &device_;
    platform::Audio &audio_;
    bool primary_;
    std::uint32_t flags_;
    WaveFormat format_;
    std::uint32_t bytes_ = 0;
    std::shared_ptr<Storage> storage_;
    platform::VoiceId voice_;
    std::int32_t volume_ = 0;
    std::int32_t pan_ = 0;
    std::uint32_t frequency_ = 0;
    bool looping_ = false;
    bool primaryPlaying_ = false;
    std::unique_ptr<Buffer3D> buffer3D_;
};

} // namespace sfcop::hle::ds3d
