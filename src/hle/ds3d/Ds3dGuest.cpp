// The guest shims of the DirectSound module: DirectSoundCreate (DSOUND.DLL ordinal 1) and the
// methods of IDirectSound, IDirectSoundBuffer, IDirectSound3DBuffer and IDirectSound3DListener,
// one per surface row, in pcrecomp's win32hle form. Each reads its arguments from the guest
// stack, reads and writes the guest's structures field by field, calls the module under one
// lock, and purges the stack its row's stack_bytes give.
//
// A guest COM object is its vtable, a reference count at +4, then (3D buffer and listener) the
// guest address of the buffer object whose count it shares, as DirectSound's 3D interfaces hold
// their buffer. The device lives until its count and its buffers' counts are all zero.
extern "C" {
#include "win32hle.h"
}

#include "hle/com/IDirectSound.h"
#include "hle/com/IDirectSound3DBuffer.h"
#include "hle/com/IDirectSound3DListener.h"
#include "hle/com/IDirectSoundBuffer.h"
#include "hle/ds3d/Buffer3D.h"
#include "hle/ds3d/DirectSound.h"
#include "hle/ds3d/Listener.h"
#include "hle/ds3d/SoundBuffer.h"
#include "platform/Audio/AudioMiniaudio.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

// The boot host's --silent and --headless flag: no sound reaches a device while it is set.
extern "C" int sfcop_mss_silent;

namespace sfcop::hle::ds3d {
namespace {

namespace com = sfcop::hle::com;

const std::uint8_t kIIDIUnknown[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                       0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};

constexpr std::uint32_t kDsCapsBytes = 96;
constexpr std::uint32_t kDsbCapsBytes = 20;
constexpr std::uint32_t kDsbDescMinBytes = 20;
constexpr std::uint32_t kWaveFormatExBytes = 18;
constexpr std::uint32_t kParamsBytes = 64; // DS3DBUFFER and DS3DLISTENER

class GuestMemory final : public Memory {
public:
    std::uint8_t *Allocate(std::size_t bytes) override {
        return static_cast<std::uint8_t *>(hle_alloc(static_cast<std::uint32_t>(bytes)));
    }
    void Free(std::uint8_t *memory) override { hle_free(memory); }
};

struct Device {
    std::unique_ptr<platform::AudioMiniaudio> audio;
    std::unique_ptr<DirectSound> sound;
    std::uint32_t buffers = 0;   // live buffer objects
    bool released = false;       // the device object's count reached zero
    std::uint32_t primary = 0;   // the primary buffer's object, while it lives
    std::uint32_t listener = 0;  // the listener's object, while the primary buffer's lives
};

struct Buffer {
    Device *device = nullptr;
    SoundBuffer *buffer = nullptr;
    std::uint32_t buffer3D = 0; // the 3D buffer's object, once asked for
};

std::mutex g_lock;
GuestMemory g_memory;
std::map<std::uint32_t, std::unique_ptr<Device>> g_devices; // by object
std::map<std::uint32_t, Buffer> g_buffers;                  // by object
std::uint32_t g_vtDevice, g_vtBuffer, g_vtBuffer3D, g_vtListener;

// ---- guest memory ------------------------------------------------------------------------

float f32(std::uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
std::uint32_t bits(float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
Vector vectorAt(std::uint32_t va) {
    return Vector{f32(MEM32(va)), f32(MEM32(va + 4)), f32(MEM32(va + 8))};
}
void putVector(std::uint32_t va, const Vector &v) {
    MEM32(va) = bits(v.x);
    MEM32(va + 4) = bits(v.y);
    MEM32(va + 8) = bits(v.z);
}
Vector vectorArgs(int first) {
    return Vector{f32(A32(first)), f32(A32(first + 1)), f32(A32(first + 2))};
}
bool iidIs(std::uint32_t iid, const void *guid) {
    return iid != 0 && std::memcmp(hle_ptr(iid), guid, 16) == 0;
}
WaveFormat formatAt(std::uint32_t va) {
    WaveFormat f;
    f.formatTag = MEM16(va);
    f.channels = MEM16(va + 2);
    f.samplesPerSec = MEM32(va + 4);
    f.avgBytesPerSec = MEM32(va + 8);
    f.blockAlign = MEM16(va + 12);
    f.bitsPerSample = MEM16(va + 14);
    return f;
}

// ---- objects -----------------------------------------------------------------------------

void buildVtables();

Device *deviceOf(std::uint32_t self) {
    auto found = g_devices.find(self);
    return found == g_devices.end() ? nullptr : found->second.get();
}
Buffer *bufferOf(std::uint32_t self) {
    auto found = g_buffers.find(self);
    return found == g_buffers.end() ? nullptr : &found->second;
}
// The buffer object a 3D buffer or listener object shares its count with.
std::uint32_t ownerOf(std::uint32_t self) { return MEM32(self + 8); }

void destroyDevice(std::uint32_t self) {
    g_devices.erase(self);
    hle_com_free(self);
}

std::uint32_t newBufferObject(Device &device, SoundBuffer *buffer) {
    std::uint32_t obj = hle_com_new(g_vtBuffer, 8);
    g_buffers[obj] = Buffer{&device, buffer, 0};
    ++device.buffers;
    return obj;
}

std::uint32_t addRef(std::uint32_t obj) { return ++MEM32(obj + 4); }

std::uint32_t releaseBuffer(std::uint32_t obj) {
    std::uint32_t left = --MEM32(obj + 4);
    if (left != 0) {
        return left;
    }
    Buffer *b = bufferOf(obj);
    if (b == nullptr) {
        return 0;
    }
    Device *device = b->device;
    if (b->buffer3D != 0) {
        hle_com_free(b->buffer3D);
    }
    if (device->primary == obj) {
        device->primary = 0;
        if (device->listener != 0) {
            hle_com_free(device->listener);
            device->listener = 0;
        }
    }
    device->sound->ReleaseBuffer(b->buffer);
    g_buffers.erase(obj);
    hle_com_free(obj);
    if (--device->buffers == 0 && device->released) {
        for (auto &entry : g_devices) {
            if (entry.second.get() == device) {
                destroyDevice(entry.first);
                break;
            }
        }
    }
    return 0;
}

// The object answering `iid` for buffer object `obj`, with its count raised, or 0.
std::uint32_t bufferInterface(std::uint32_t obj, std::uint32_t iid) {
    Buffer *b = bufferOf(obj);
    if (b == nullptr) {
        return 0;
    }
    if (iidIs(iid, &com::kIID_IDirectSoundBuffer) || iidIs(iid, kIIDIUnknown)) {
        addRef(obj);
        return obj;
    }
    if (iidIs(iid, &com::kIID_IDirectSound3DBuffer) && b->buffer->Get3D() != nullptr) {
        if (b->buffer3D == 0) {
            b->buffer3D = hle_com_new(g_vtBuffer3D, 12);
            MEM32(b->buffer3D + 8) = obj;
        }
        addRef(obj);
        return b->buffer3D;
    }
    if (iidIs(iid, &com::kIID_IDirectSound3DListener) && b->device->primary == obj &&
        b->device->sound->GetListener() != nullptr) {
        if (b->device->listener == 0) {
            b->device->listener = hle_com_new(g_vtListener, 12);
            MEM32(b->device->listener + 8) = obj;
        }
        addRef(obj);
        return b->device->listener;
    }
    return 0;
}

void answerInterface(std::uint32_t found, std::uint32_t out) {
    if (out != 0) {
        MEM32(out) = found;
    }
    RET(found != 0 ? kDsOk : kENoInterface, 3);
}

// ---- DirectSoundCreate -------------------------------------------------------------------

void s_DirectSoundCreate() { // (guid, &out, outer)
    std::lock_guard<std::mutex> guard(g_lock);
    std::uint32_t out = A32(1);
    if (out == 0 || A32(2) != 0) {
        RET(kDsErrInvalidParam, 3);
    }
    MEM32(out) = 0;
    buildVtables();
    auto device = std::make_unique<Device>();
    device->audio = std::make_unique<platform::AudioMiniaudio>(
        sfcop_mss_silent ? platform::AudioOutput::kNullDevice : platform::AudioOutput::kDevice);
    device->sound = std::make_unique<DirectSound>(*device->audio, g_memory);
    HResult opened = device->sound->Open();
    if (opened != kDsOk) {
        RET(opened, 3);
    }
    std::uint32_t obj = hle_com_new(g_vtDevice, 8);
    g_devices[obj] = std::move(device);
    MEM32(out) = obj;
    RET(kDsOk, 3);
}

// ---- IDirectSound ------------------------------------------------------------------------

void d_QueryInterface() { // (this, iid, &out)
    std::lock_guard<std::mutex> guard(g_lock);
    std::uint32_t self = A32(0), iid = A32(1);
    std::uint32_t found = 0;
    if (deviceOf(self) != nullptr &&
        (iidIs(iid, &com::kIID_IDirectSound) || iidIs(iid, kIIDIUnknown))) {
        addRef(self);
        found = self;
    }
    answerInterface(found, A32(2));
}
void d_AddRef() {
    std::lock_guard<std::mutex> guard(g_lock);
    RET(addRef(A32(0)), 1);
}
void d_Release() {
    std::lock_guard<std::mutex> guard(g_lock);
    std::uint32_t self = A32(0);
    std::uint32_t left = --MEM32(self + 4);
    Device *device = deviceOf(self);
    if (left == 0 && device != nullptr) {
        device->released = true;
        if (device->buffers == 0) {
            destroyDevice(self);
        }
    }
    RET(left, 1);
}
void d_CreateSoundBuffer() { // (this, desc, &out, outer)
    std::lock_guard<std::mutex> guard(g_lock);
    Device *device = deviceOf(A32(0));
    std::uint32_t desc = A32(1), out = A32(2);
    if (device == nullptr || desc == 0 || out == 0 || A32(3) != 0 ||
        MEM32(desc) < kDsbDescMinBytes) {
        RET(kDsErrInvalidParam, 4);
    }
    MEM32(out) = 0;
    BufferDesc d;
    d.flags = MEM32(desc + 4);
    d.bufferBytes = MEM32(desc + 8);
    std::uint32_t format = MEM32(desc + 16);
    d.hasFormat = format != 0;
    if (d.hasFormat) {
        d.format = formatAt(format);
    }
    SoundBuffer *buffer = nullptr;
    HResult made = device->sound->CreateSoundBuffer(d, &buffer);
    if (made != kDsOk) {
        RET(made, 4);
    }
    std::uint32_t obj = 0;
    if (buffer->IsPrimary()) {
        if (device->primary == 0) {
            device->primary = newBufferObject(*device, buffer);
        } else {
            addRef(device->primary);
        }
        obj = device->primary;
    } else {
        obj = newBufferObject(*device, buffer);
    }
    MEM32(out) = obj;
    RET(kDsOk, 4);
}
void d_GetCaps() { // (this, caps)
    std::lock_guard<std::mutex> guard(g_lock);
    Device *device = deviceOf(A32(0));
    std::uint32_t caps = A32(1);
    if (device == nullptr || caps == 0 || MEM32(caps) != kDsCapsBytes) {
        RET(kDsErrInvalidParam, 2);
    }
    Caps c = device->sound->GetCaps();
    std::memset(hle_ptr(caps + 4), 0, kDsCapsBytes - 4);
    MEM32(caps + 4) = c.flags;
    MEM32(caps + 8) = c.minSecondarySampleRate;
    MEM32(caps + 12) = c.maxSecondarySampleRate;
    MEM32(caps + 16) = c.primaryBuffers;
    MEM32(caps + 20) = c.maxHwMixingAllBuffers;
    MEM32(caps + 24) = c.maxHwMixingStaticBuffers;
    MEM32(caps + 28) = c.maxHwMixingStreamingBuffers;
    MEM32(caps + 32) = c.freeHwMixingAllBuffers;
    MEM32(caps + 36) = c.freeHwMixingStaticBuffers;
    MEM32(caps + 40) = c.freeHwMixingStreamingBuffers;
    RET(kDsOk, 2);
}
void d_DuplicateSoundBuffer() { // (this, source, &out)
    std::lock_guard<std::mutex> guard(g_lock);
    Device *device = deviceOf(A32(0));
    Buffer *source = bufferOf(A32(1));
    std::uint32_t out = A32(2);
    if (device == nullptr || source == nullptr || out == 0 || source->device != device) {
        RET(kDsErrInvalidParam, 3);
    }
    MEM32(out) = 0;
    SoundBuffer *copy = nullptr;
    HResult made = device->sound->DuplicateSoundBuffer(*source->buffer, &copy);
    if (made != kDsOk) {
        RET(made, 3);
    }
    MEM32(out) = newBufferObject(*device, copy);
    RET(kDsOk, 3);
}
void d_SetCooperativeLevel() { // (this, hwnd, level)
    std::lock_guard<std::mutex> guard(g_lock);
    Device *device = deviceOf(A32(0));
    if (device == nullptr) {
        RET(kDsErrInvalidParam, 3);
    }
    RET(device->sound->SetCooperativeLevel(A32(1) != 0, A32(2)), 3);
}
void d_Compact() {
    std::lock_guard<std::mutex> guard(g_lock);
    Device *device = deviceOf(A32(0));
    RET(device != nullptr ? device->sound->Compact() : kDsErrInvalidParam, 1);
}
void d_GetSpeakerConfig() { // (this, &config)
    std::lock_guard<std::mutex> guard(g_lock);
    Device *device = deviceOf(A32(0));
    if (device == nullptr || A32(1) == 0) {
        RET(kDsErrInvalidParam, 2);
    }
    MEM32(A32(1)) = device->sound->GetSpeakerConfig();
    RET(kDsOk, 2);
}
void d_SetSpeakerConfig() { // (this, config)
    std::lock_guard<std::mutex> guard(g_lock);
    Device *device = deviceOf(A32(0));
    RET(device != nullptr ? device->sound->SetSpeakerConfig(A32(1)) : kDsErrInvalidParam, 2);
}
void d_Initialize() { // (this, guid)
    std::lock_guard<std::mutex> guard(g_lock);
    Device *device = deviceOf(A32(0));
    RET(device != nullptr ? device->sound->Initialize() : kDsErrInvalidParam, 2);
}

// ---- IDirectSoundBuffer ------------------------------------------------------------------

#define SFCOP_BUFFER_OR_FAIL(argc)                                                             \
    std::lock_guard<std::mutex> guard(g_lock);                                                \
    Buffer *b = bufferOf(A32(0));                                                              \
    if (b == nullptr) {                                                                        \
        RET(kDsErrInvalidParam, argc);                                                         \
    }                                                                                          \
    SoundBuffer &buffer = *b->buffer

void b_QueryInterface() { // (this, iid, &out)
    std::lock_guard<std::mutex> guard(g_lock);
    answerInterface(bufferInterface(A32(0), A32(1)), A32(2));
}
void b_AddRef() {
    std::lock_guard<std::mutex> guard(g_lock);
    RET(addRef(A32(0)), 1);
}
void b_Release() {
    std::lock_guard<std::mutex> guard(g_lock);
    RET(releaseBuffer(A32(0)), 1);
}
void b_GetCaps() { // (this, caps)
    SFCOP_BUFFER_OR_FAIL(2);
    std::uint32_t caps = A32(1);
    if (caps == 0 || MEM32(caps) != kDsbCapsBytes) {
        RET(kDsErrInvalidParam, 2);
    }
    BufferCaps c = buffer.GetCaps();
    MEM32(caps + 4) = c.flags;
    MEM32(caps + 8) = c.bufferBytes;
    MEM32(caps + 12) = c.unlockTransferRate;
    MEM32(caps + 16) = c.playCpuOverhead;
    RET(kDsOk, 2);
}
void b_GetCurrentPosition() { // (this, &play, &write)
    SFCOP_BUFFER_OR_FAIL(3);
    std::uint32_t play = 0, write = 0;
    HResult got = buffer.GetCurrentPosition(&play, &write);
    if (got == kDsOk && A32(1) != 0) {
        MEM32(A32(1)) = play;
    }
    if (got == kDsOk && A32(2) != 0) {
        MEM32(A32(2)) = write;
    }
    RET(got, 3);
}
void b_GetFormat() { // (this, format, bytes, &written)
    SFCOP_BUFFER_OR_FAIL(4);
    std::uint32_t format = A32(1), bytes = A32(2), written = A32(3);
    if (format == 0) {
        if (written == 0) {
            RET(kDsErrInvalidParam, 4);
        }
        MEM32(written) = kWaveFormatExBytes;
        RET(kDsOk, 4);
    }
    if (bytes < kWaveFormatExBytes - 2) {
        RET(kDsErrInvalidParam, 4);
    }
    WaveFormat f = buffer.GetFormat();
    MEM16(format) = f.formatTag;
    MEM16(format + 2) = f.channels;
    MEM32(format + 4) = f.samplesPerSec;
    MEM32(format + 8) = f.avgBytesPerSec;
    MEM16(format + 12) = f.blockAlign;
    MEM16(format + 14) = f.bitsPerSample;
    std::uint32_t copied = kWaveFormatExBytes - 2;
    if (bytes >= kWaveFormatExBytes) {
        MEM16(format + 16) = 0; // cbSize
        copied = kWaveFormatExBytes;
    }
    if (written != 0) {
        MEM32(written) = copied;
    }
    RET(kDsOk, 4);
}
void b_GetVolume() { // (this, &volume)
    SFCOP_BUFFER_OR_FAIL(2);
    std::int32_t volume = 0;
    HResult got = A32(1) != 0 ? buffer.GetVolume(&volume) : kDsErrInvalidParam;
    if (got == kDsOk) {
        MEM32(A32(1)) = static_cast<std::uint32_t>(volume);
    }
    RET(got, 2);
}
void b_GetPan() { // (this, &pan)
    SFCOP_BUFFER_OR_FAIL(2);
    std::int32_t pan = 0;
    HResult got = A32(1) != 0 ? buffer.GetPan(&pan) : kDsErrInvalidParam;
    if (got == kDsOk) {
        MEM32(A32(1)) = static_cast<std::uint32_t>(pan);
    }
    RET(got, 2);
}
void b_GetFrequency() { // (this, &hz)
    SFCOP_BUFFER_OR_FAIL(2);
    std::uint32_t hz = 0;
    HResult got = A32(1) != 0 ? buffer.GetFrequency(&hz) : kDsErrInvalidParam;
    if (got == kDsOk) {
        MEM32(A32(1)) = hz;
    }
    RET(got, 2);
}
void b_GetStatus() { // (this, &status)
    SFCOP_BUFFER_OR_FAIL(2);
    std::uint32_t status = 0;
    HResult got = A32(1) != 0 ? buffer.GetStatus(&status) : kDsErrInvalidParam;
    if (got == kDsOk) {
        MEM32(A32(1)) = status;
    }
    RET(got, 2);
}
void b_Initialize() { // (this, device, desc)
    SFCOP_BUFFER_OR_FAIL(3);
    RET(buffer.Initialize(), 3);
}
void b_Lock() { // (this, offset, bytes, &p1, &n1, &p2, &n2, flags)
    SFCOP_BUFFER_OR_FAIL(8);
    if (A32(3) == 0 || A32(4) == 0) {
        RET(kDsErrInvalidParam, 8);
    }
    LockedRange range;
    HResult locked = buffer.Lock(A32(1), A32(2), A32(7), &range);
    if (locked != kDsOk) {
        RET(locked, 8);
    }
    MEM32(A32(3)) = hle_va(range.first);
    MEM32(A32(4)) = range.firstBytes;
    if (A32(5) != 0) {
        MEM32(A32(5)) = range.second != nullptr ? hle_va(range.second) : 0;
    }
    if (A32(6) != 0) {
        MEM32(A32(6)) = range.secondBytes;
    }
    RET(kDsOk, 8);
}
void b_Play() { // (this, reserved, priority, flags)
    SFCOP_BUFFER_OR_FAIL(4);
    RET(buffer.Play(A32(3)), 4);
}
void b_SetCurrentPosition() { // (this, offset)
    SFCOP_BUFFER_OR_FAIL(2);
    RET(buffer.SetCurrentPosition(A32(1)), 2);
}
void b_SetFormat() { // (this, format)
    SFCOP_BUFFER_OR_FAIL(2);
    if (A32(1) == 0) {
        RET(kDsErrInvalidParam, 2);
    }
    RET(buffer.SetFormat(formatAt(A32(1))), 2);
}
void b_SetVolume() { // (this, volume)
    SFCOP_BUFFER_OR_FAIL(2);
    RET(buffer.SetVolume(static_cast<std::int32_t>(A32(1))), 2);
}
void b_SetPan() { // (this, pan)
    SFCOP_BUFFER_OR_FAIL(2);
    RET(buffer.SetPan(static_cast<std::int32_t>(A32(1))), 2);
}
void b_SetFrequency() { // (this, hz)
    SFCOP_BUFFER_OR_FAIL(2);
    RET(buffer.SetFrequency(A32(1)), 2);
}
void b_Stop() {
    SFCOP_BUFFER_OR_FAIL(1);
    RET(buffer.Stop(), 1);
}
void b_Unlock() { // (this, p1, n1, p2, n2)
    SFCOP_BUFFER_OR_FAIL(5);
    LockedRange range;
    range.first = static_cast<std::uint8_t *>(hle_ptr(A32(1)));
    range.firstBytes = A32(2);
    range.second = static_cast<std::uint8_t *>(hle_ptr(A32(3)));
    range.secondBytes = A32(4);
    RET(buffer.Unlock(range), 5);
}
void b_Restore() {
    SFCOP_BUFFER_OR_FAIL(1);
    RET(buffer.Restore(), 1);
}

// ---- IDirectSound3DBuffer ----------------------------------------------------------------

#define SFCOP_3D_OR_FAIL(argc)                                                                 \
    std::lock_guard<std::mutex> guard(g_lock);                                                \
    Buffer *b = bufferOf(ownerOf(A32(0)));                                                     \
    if (b == nullptr || b->buffer->Get3D() == nullptr) {                                       \
        RET(kDsErrInvalidParam, argc);                                                         \
    }                                                                                          \
    Buffer3D &b3d = *b->buffer->Get3D()

void putBuffer3D(std::uint32_t va, const Buffer3DParams &p) {
    putVector(va + 4, p.position);
    putVector(va + 16, p.velocity);
    MEM32(va + 28) = p.insideConeAngle;
    MEM32(va + 32) = p.outsideConeAngle;
    putVector(va + 36, p.coneOrientation);
    MEM32(va + 48) = static_cast<std::uint32_t>(p.coneOutsideVolume);
    MEM32(va + 52) = bits(p.minDistance);
    MEM32(va + 56) = bits(p.maxDistance);
    MEM32(va + 60) = p.mode;
}
Buffer3DParams buffer3DAt(std::uint32_t va) {
    Buffer3DParams p;
    p.position = vectorAt(va + 4);
    p.velocity = vectorAt(va + 16);
    p.insideConeAngle = MEM32(va + 28);
    p.outsideConeAngle = MEM32(va + 32);
    p.coneOrientation = vectorAt(va + 36);
    p.coneOutsideVolume = static_cast<std::int32_t>(MEM32(va + 48));
    p.minDistance = f32(MEM32(va + 52));
    p.maxDistance = f32(MEM32(va + 56));
    p.mode = MEM32(va + 60);
    return p;
}

void t_QueryInterface() { // (this, iid, &out)
    std::lock_guard<std::mutex> guard(g_lock);
    std::uint32_t self = A32(0), iid = A32(1);
    std::uint32_t found = 0;
    if (iidIs(iid, &com::kIID_IDirectSound3DBuffer)) {
        addRef(ownerOf(self));
        found = self;
    } else {
        found = bufferInterface(ownerOf(self), iid);
    }
    answerInterface(found, A32(2));
}
void t_AddRef() {
    std::lock_guard<std::mutex> guard(g_lock);
    RET(addRef(ownerOf(A32(0))), 1);
}
void t_Release() {
    std::lock_guard<std::mutex> guard(g_lock);
    RET(releaseBuffer(ownerOf(A32(0))), 1);
}
void t_GetAllParameters() { // (this, params)
    SFCOP_3D_OR_FAIL(2);
    if (A32(1) == 0 || MEM32(A32(1)) < kParamsBytes) {
        RET(kDsErrInvalidParam, 2);
    }
    putBuffer3D(A32(1), b3d.Parameters());
    RET(kDsOk, 2);
}
void t_GetConeAngles() { // (this, &inside, &outside)
    SFCOP_3D_OR_FAIL(3);
    if (A32(1) == 0 && A32(2) == 0) {
        RET(kDsErrInvalidParam, 3);
    }
    if (A32(1) != 0) {
        MEM32(A32(1)) = b3d.Parameters().insideConeAngle;
    }
    if (A32(2) != 0) {
        MEM32(A32(2)) = b3d.Parameters().outsideConeAngle;
    }
    RET(kDsOk, 3);
}
#define SFCOP_3D_GET_VECTOR(name, field)                                                       \
    void name() {                                                                              \
        SFCOP_3D_OR_FAIL(2);                                                                   \
        if (A32(1) == 0) {                                                                     \
            RET(kDsErrInvalidParam, 2);                                                        \
        }                                                                                      \
        putVector(A32(1), b3d.Parameters().field);                                             \
        RET(kDsOk, 2);                                                                         \
    }
#define SFCOP_3D_GET_DWORD(name, value)                                                        \
    void name() {                                                                              \
        SFCOP_3D_OR_FAIL(2);                                                                   \
        if (A32(1) == 0) {                                                                     \
            RET(kDsErrInvalidParam, 2);                                                        \
        }                                                                                      \
        MEM32(A32(1)) = (value);                                                               \
        RET(kDsOk, 2);                                                                         \
    }
SFCOP_3D_GET_VECTOR(t_GetConeOrientation, coneOrientation)
SFCOP_3D_GET_VECTOR(t_GetPosition, position)
SFCOP_3D_GET_VECTOR(t_GetVelocity, velocity)
SFCOP_3D_GET_DWORD(t_GetConeOutsideVolume,
                   static_cast<std::uint32_t>(b3d.Parameters().coneOutsideVolume))
SFCOP_3D_GET_DWORD(t_GetMaxDistance, bits(b3d.Parameters().maxDistance))
SFCOP_3D_GET_DWORD(t_GetMinDistance, bits(b3d.Parameters().minDistance))
SFCOP_3D_GET_DWORD(t_GetMode, b3d.Parameters().mode)

void t_SetAllParameters() { // (this, params, apply)
    SFCOP_3D_OR_FAIL(3);
    if (A32(1) == 0 || MEM32(A32(1)) < kParamsBytes) {
        RET(kDsErrInvalidParam, 3);
    }
    RET(b3d.SetAllParameters(buffer3DAt(A32(1)), A32(2)), 3);
}
void t_SetConeAngles() { // (this, inside, outside, apply)
    SFCOP_3D_OR_FAIL(4);
    RET(b3d.SetConeAngles(A32(1), A32(2), A32(3)), 4);
}
void t_SetConeOrientation() { // (this, x, y, z, apply)
    SFCOP_3D_OR_FAIL(5);
    RET(b3d.SetConeOrientation(vectorArgs(1), A32(4)), 5);
}
void t_SetConeOutsideVolume() { // (this, volume, apply)
    SFCOP_3D_OR_FAIL(3);
    RET(b3d.SetConeOutsideVolume(static_cast<std::int32_t>(A32(1)), A32(2)), 3);
}
void t_SetMaxDistance() { // (this, distance, apply)
    SFCOP_3D_OR_FAIL(3);
    RET(b3d.SetMaxDistance(f32(A32(1)), A32(2)), 3);
}
void t_SetMinDistance() { // (this, distance, apply)
    SFCOP_3D_OR_FAIL(3);
    RET(b3d.SetMinDistance(f32(A32(1)), A32(2)), 3);
}
void t_SetMode() { // (this, mode, apply)
    SFCOP_3D_OR_FAIL(3);
    RET(b3d.SetMode(A32(1), A32(2)), 3);
}
void t_SetPosition() { // (this, x, y, z, apply)
    SFCOP_3D_OR_FAIL(5);
    RET(b3d.SetPosition(vectorArgs(1), A32(4)), 5);
}
void t_SetVelocity() { // (this, x, y, z, apply)
    SFCOP_3D_OR_FAIL(5);
    RET(b3d.SetVelocity(vectorArgs(1), A32(4)), 5);
}

// ---- IDirectSound3DListener --------------------------------------------------------------

#define SFCOP_LISTENER_OR_FAIL(argc)                                                           \
    std::lock_guard<std::mutex> guard(g_lock);                                                \
    Buffer *b = bufferOf(ownerOf(A32(0)));                                                     \
    if (b == nullptr || b->device->sound->GetListener() == nullptr) {                          \
        RET(kDsErrInvalidParam, argc);                                                         \
    }                                                                                          \
    Listener &listener = *b->device->sound->GetListener()

void putListener(std::uint32_t va, const ListenerParams &p) {
    putVector(va + 4, p.position);
    putVector(va + 16, p.velocity);
    putVector(va + 28, p.front);
    putVector(va + 40, p.top);
    MEM32(va + 52) = bits(p.distanceFactor);
    MEM32(va + 56) = bits(p.rolloffFactor);
    MEM32(va + 60) = bits(p.dopplerFactor);
}
ListenerParams listenerAt(std::uint32_t va) {
    ListenerParams p;
    p.position = vectorAt(va + 4);
    p.velocity = vectorAt(va + 16);
    p.front = vectorAt(va + 28);
    p.top = vectorAt(va + 40);
    p.distanceFactor = f32(MEM32(va + 52));
    p.rolloffFactor = f32(MEM32(va + 56));
    p.dopplerFactor = f32(MEM32(va + 60));
    return p;
}

void l_QueryInterface() { // (this, iid, &out)
    std::lock_guard<std::mutex> guard(g_lock);
    std::uint32_t self = A32(0), iid = A32(1);
    std::uint32_t found = 0;
    if (iidIs(iid, &com::kIID_IDirectSound3DListener)) {
        addRef(ownerOf(self));
        found = self;
    } else {
        found = bufferInterface(ownerOf(self), iid);
    }
    answerInterface(found, A32(2));
}
void l_AddRef() {
    std::lock_guard<std::mutex> guard(g_lock);
    RET(addRef(ownerOf(A32(0))), 1);
}
void l_Release() {
    std::lock_guard<std::mutex> guard(g_lock);
    RET(releaseBuffer(ownerOf(A32(0))), 1);
}
void l_GetAllParameters() { // (this, params)
    SFCOP_LISTENER_OR_FAIL(2);
    if (A32(1) == 0 || MEM32(A32(1)) < kParamsBytes) {
        RET(kDsErrInvalidParam, 2);
    }
    putListener(A32(1), listener.Parameters());
    RET(kDsOk, 2);
}
#define SFCOP_LISTENER_GET_FLOAT(name, field)                                                  \
    void name() {                                                                              \
        SFCOP_LISTENER_OR_FAIL(2);                                                             \
        if (A32(1) == 0) {                                                                     \
            RET(kDsErrInvalidParam, 2);                                                        \
        }                                                                                      \
        MEM32(A32(1)) = bits(listener.Parameters().field);                                     \
        RET(kDsOk, 2);                                                                         \
    }
#define SFCOP_LISTENER_GET_VECTOR(name, field)                                                 \
    void name() {                                                                              \
        SFCOP_LISTENER_OR_FAIL(2);                                                             \
        if (A32(1) == 0) {                                                                     \
            RET(kDsErrInvalidParam, 2);                                                        \
        }                                                                                      \
        putVector(A32(1), listener.Parameters().field);                                        \
        RET(kDsOk, 2);                                                                         \
    }
SFCOP_LISTENER_GET_FLOAT(l_GetDistanceFactor, distanceFactor)
SFCOP_LISTENER_GET_FLOAT(l_GetDopplerFactor, dopplerFactor)
SFCOP_LISTENER_GET_FLOAT(l_GetRolloffFactor, rolloffFactor)
SFCOP_LISTENER_GET_VECTOR(l_GetPosition, position)
SFCOP_LISTENER_GET_VECTOR(l_GetVelocity, velocity)

void l_GetOrientation() { // (this, front, top)
    SFCOP_LISTENER_OR_FAIL(3);
    if (A32(1) == 0 || A32(2) == 0) {
        RET(kDsErrInvalidParam, 3);
    }
    putVector(A32(1), listener.Parameters().front);
    putVector(A32(2), listener.Parameters().top);
    RET(kDsOk, 3);
}
void l_SetAllParameters() { // (this, params, apply)
    SFCOP_LISTENER_OR_FAIL(3);
    if (A32(1) == 0 || MEM32(A32(1)) < kParamsBytes) {
        RET(kDsErrInvalidParam, 3);
    }
    RET(listener.SetAllParameters(listenerAt(A32(1)), A32(2)), 3);
}
void l_SetDistanceFactor() { // (this, factor, apply)
    SFCOP_LISTENER_OR_FAIL(3);
    RET(listener.SetDistanceFactor(f32(A32(1)), A32(2)), 3);
}
void l_SetDopplerFactor() { // (this, factor, apply)
    SFCOP_LISTENER_OR_FAIL(3);
    RET(listener.SetDopplerFactor(f32(A32(1)), A32(2)), 3);
}
void l_SetOrientation() { // (this, fx, fy, fz, tx, ty, tz, apply)
    SFCOP_LISTENER_OR_FAIL(8);
    RET(listener.SetOrientation(vectorArgs(1), vectorArgs(4), A32(7)), 8);
}
void l_SetPosition() { // (this, x, y, z, apply)
    SFCOP_LISTENER_OR_FAIL(5);
    RET(listener.SetPosition(vectorArgs(1), A32(4)), 5);
}
void l_SetRolloffFactor() { // (this, factor, apply)
    SFCOP_LISTENER_OR_FAIL(3);
    RET(listener.SetRolloffFactor(f32(A32(1)), A32(2)), 3);
}
void l_SetVelocity() { // (this, x, y, z, apply)
    SFCOP_LISTENER_OR_FAIL(5);
    RET(listener.SetVelocity(vectorArgs(1), A32(4)), 5);
}
void l_CommitDeferredSettings() {
    SFCOP_LISTENER_OR_FAIL(1);
    RET(listener.CommitDeferredSettings(), 1);
}

// ---- tables ------------------------------------------------------------------------------

// Each interface's methods in slot order (src/hle/com/IDirectSound*.h).
#define SFCOP_DEVICE_METHODS(X)                                                                \
    X("IDirectSound::QueryInterface", d_QueryInterface)                                        \
    X("IDirectSound::AddRef", d_AddRef)                                                        \
    X("IDirectSound::Release", d_Release)                                                      \
    X("IDirectSound::CreateSoundBuffer", d_CreateSoundBuffer)                                  \
    X("IDirectSound::GetCaps", d_GetCaps)                                                      \
    X("IDirectSound::DuplicateSoundBuffer", d_DuplicateSoundBuffer)                            \
    X("IDirectSound::SetCooperativeLevel", d_SetCooperativeLevel)                              \
    X("IDirectSound::Compact", d_Compact)                                                      \
    X("IDirectSound::GetSpeakerConfig", d_GetSpeakerConfig)                                    \
    X("IDirectSound::SetSpeakerConfig", d_SetSpeakerConfig)                                    \
    X("IDirectSound::Initialize", d_Initialize)
#define SFCOP_BUFFER_METHODS(X)                                                                \
    X("IDirectSoundBuffer::QueryInterface", b_QueryInterface)                                  \
    X("IDirectSoundBuffer::AddRef", b_AddRef)                                                  \
    X("IDirectSoundBuffer::Release", b_Release)                                                \
    X("IDirectSoundBuffer::GetCaps", b_GetCaps)                                                \
    X("IDirectSoundBuffer::GetCurrentPosition", b_GetCurrentPosition)                          \
    X("IDirectSoundBuffer::GetFormat", b_GetFormat)                                            \
    X("IDirectSoundBuffer::GetVolume", b_GetVolume)                                            \
    X("IDirectSoundBuffer::GetPan", b_GetPan)                                                  \
    X("IDirectSoundBuffer::GetFrequency", b_GetFrequency)                                      \
    X("IDirectSoundBuffer::GetStatus", b_GetStatus)                                            \
    X("IDirectSoundBuffer::Initialize", b_Initialize)                                          \
    X("IDirectSoundBuffer::Lock", b_Lock)                                                      \
    X("IDirectSoundBuffer::Play", b_Play)                                                      \
    X("IDirectSoundBuffer::SetCurrentPosition", b_SetCurrentPosition)                          \
    X("IDirectSoundBuffer::SetFormat", b_SetFormat)                                            \
    X("IDirectSoundBuffer::SetVolume", b_SetVolume)                                            \
    X("IDirectSoundBuffer::SetPan", b_SetPan)                                                  \
    X("IDirectSoundBuffer::SetFrequency", b_SetFrequency)                                      \
    X("IDirectSoundBuffer::Stop", b_Stop)                                                      \
    X("IDirectSoundBuffer::Unlock", b_Unlock)                                                  \
    X("IDirectSoundBuffer::Restore", b_Restore)
#define SFCOP_BUFFER3D_METHODS(X)                                                              \
    X("IDirectSound3DBuffer::QueryInterface", t_QueryInterface)                                \
    X("IDirectSound3DBuffer::AddRef", t_AddRef)                                                \
    X("IDirectSound3DBuffer::Release", t_Release)                                              \
    X("IDirectSound3DBuffer::GetAllParameters", t_GetAllParameters)                            \
    X("IDirectSound3DBuffer::GetConeAngles", t_GetConeAngles)                                  \
    X("IDirectSound3DBuffer::GetConeOrientation", t_GetConeOrientation)                        \
    X("IDirectSound3DBuffer::GetConeOutsideVolume", t_GetConeOutsideVolume)                    \
    X("IDirectSound3DBuffer::GetMaxDistance", t_GetMaxDistance)                                \
    X("IDirectSound3DBuffer::GetMinDistance", t_GetMinDistance)                                \
    X("IDirectSound3DBuffer::GetMode", t_GetMode)                                              \
    X("IDirectSound3DBuffer::GetPosition", t_GetPosition)                                      \
    X("IDirectSound3DBuffer::GetVelocity", t_GetVelocity)                                      \
    X("IDirectSound3DBuffer::SetAllParameters", t_SetAllParameters)                            \
    X("IDirectSound3DBuffer::SetConeAngles", t_SetConeAngles)                                  \
    X("IDirectSound3DBuffer::SetConeOrientation", t_SetConeOrientation)                        \
    X("IDirectSound3DBuffer::SetConeOutsideVolume", t_SetConeOutsideVolume)                    \
    X("IDirectSound3DBuffer::SetMaxDistance", t_SetMaxDistance)                                \
    X("IDirectSound3DBuffer::SetMinDistance", t_SetMinDistance)                                \
    X("IDirectSound3DBuffer::SetMode", t_SetMode)                                              \
    X("IDirectSound3DBuffer::SetPosition", t_SetPosition)                                      \
    X("IDirectSound3DBuffer::SetVelocity", t_SetVelocity)
#define SFCOP_LISTENER_METHODS(X)                                                              \
    X("IDirectSound3DListener::QueryInterface", l_QueryInterface)                              \
    X("IDirectSound3DListener::AddRef", l_AddRef)                                              \
    X("IDirectSound3DListener::Release", l_Release)                                            \
    X("IDirectSound3DListener::GetAllParameters", l_GetAllParameters)                          \
    X("IDirectSound3DListener::GetDistanceFactor", l_GetDistanceFactor)                        \
    X("IDirectSound3DListener::GetDopplerFactor", l_GetDopplerFactor)                          \
    X("IDirectSound3DListener::GetOrientation", l_GetOrientation)                              \
    X("IDirectSound3DListener::GetPosition", l_GetPosition)                                    \
    X("IDirectSound3DListener::GetRolloffFactor", l_GetRolloffFactor)                          \
    X("IDirectSound3DListener::GetVelocity", l_GetVelocity)                                    \
    X("IDirectSound3DListener::SetAllParameters", l_SetAllParameters)                          \
    X("IDirectSound3DListener::SetDistanceFactor", l_SetDistanceFactor)                        \
    X("IDirectSound3DListener::SetDopplerFactor", l_SetDopplerFactor)                          \
    X("IDirectSound3DListener::SetOrientation", l_SetOrientation)                              \
    X("IDirectSound3DListener::SetPosition", l_SetPosition)                                    \
    X("IDirectSound3DListener::SetRolloffFactor", l_SetRolloffFactor)                          \
    X("IDirectSound3DListener::SetVelocity", l_SetVelocity)                                    \
    X("IDirectSound3DListener::CommitDeferredSettings", l_CommitDeferredSettings)

#define SFCOP_SHIM(name, fn) {name, fn},

const win32hle_shim kDeviceMethods[] = {SFCOP_DEVICE_METHODS(SFCOP_SHIM){nullptr, nullptr}};
const win32hle_shim kBufferMethods[] = {SFCOP_BUFFER_METHODS(SFCOP_SHIM){nullptr, nullptr}};
const win32hle_shim kBuffer3DMethods[] = {SFCOP_BUFFER3D_METHODS(SFCOP_SHIM){nullptr, nullptr}};
const win32hle_shim kListenerMethods[] = {SFCOP_LISTENER_METHODS(SFCOP_SHIM){nullptr, nullptr}};

// Every method name is in sfcop_hle_ds3d[] as well, registered before any other module's, so
// these vtables resolve to this file's shims.
void buildVtables() {
    if (g_vtDevice != 0) {
        return;
    }
    g_vtDevice = hle_com_vtable(kDeviceMethods);
    g_vtBuffer = hle_com_vtable(kBufferMethods);
    g_vtBuffer3D = hle_com_vtable(kBuffer3DMethods);
    g_vtListener = hle_com_vtable(kListenerMethods);
}

} // namespace
} // namespace sfcop::hle::ds3d

using namespace sfcop::hle::ds3d;

extern "C" const win32hle_shim sfcop_hle_ds3d[] = {
    {"dsound.dll#1", s_DirectSoundCreate},
    {"DirectSoundCreate", s_DirectSoundCreate},
    SFCOP_DEVICE_METHODS(SFCOP_SHIM) SFCOP_BUFFER_METHODS(SFCOP_SHIM)
        SFCOP_BUFFER3D_METHODS(SFCOP_SHIM) SFCOP_LISTENER_METHODS(SFCOP_SHIM){nullptr, nullptr}};
