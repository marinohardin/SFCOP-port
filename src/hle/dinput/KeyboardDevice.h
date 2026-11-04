// IDirectInputDeviceA for the system keyboard (DESIGN §5.7), on the Input component: immediate
// reads (GetDeviceState) from Input's keyboard state, buffered reads (GetDeviceData) from Input's
// event queue, and a notification event through Threads.
//
// Each method is the COM method of the same name with the guest's pointers already read: a null
// guest pointer arrives as nullptr, a structure as its host form (DInputDefs.h), and the method
// returns the HRESULT the game reads. Methods may be called from any thread.
#pragma once

#include "hle/dinput/DInputDefs.h"
#include "hle/dinput/KeyboardHub.h"

#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <optional>

namespace sfcop::hle::dinput {

// A DIPROPHEADER: the size of the whole property structure, the header's own size, the object
// and how `obj` names it.
struct PropertyHeader {
    std::uint32_t size = 0;
    std::uint32_t headerSize = 0;
    std::uint32_t obj = 0;
    std::uint32_t how = 0;
};

class KeyboardDevice {
public:
    // COM lifetime: DirectInput::CreateDevice makes one with `new` and a count of 1; the Release()
    // that brings the count to 0 deletes it.
    explicit KeyboardDevice(std::shared_ptr<KeyboardHub> hub);
    KeyboardDevice(const KeyboardDevice &) = delete;
    KeyboardDevice &operator=(const KeyboardDevice &) = delete;

    std::uint32_t AddRef();
    std::uint32_t Release();

    // `size` is the caller's dwSize: the DirectX 3 or the full DIDEVCAPS. Fills attached,
    // keyboard type, no axes, one button per key. DIERR_INVALIDPARAM for a null or a wrong size.
    HResult GetCapabilities(std::uint32_t size, DeviceCaps *caps);

    // Calls `callback` for each key (as GetObjectInfo describes it) in DIK_ order while it
    // returns true, when `flags` asks for all objects or for buttons; for any other type it
    // calls nothing. DIERR_INVALIDPARAM for an empty callback.
    HResult EnumObjects(const std::function<bool(const ObjectInstance &)> &callback,
                        std::uint32_t flags);

    // DIPROP_BUFFERSIZE on the device (a DIPROPDWORD, DIPH_DEVICE) reads or sets how many
    // records the buffer holds; 0 means not buffered. Another predefined property is
    // DIERR_UNSUPPORTED; a malformed header DIERR_INVALIDPARAM; setting while acquired
    // DIERR_ACQUIRED.
    HResult GetProperty(std::uint32_t property, const PropertyHeader &header, std::uint32_t *value);
    HResult SetProperty(std::uint32_t property, const PropertyHeader &header, std::uint32_t value);

    // Acquire empties the buffer and starts collecting; DI_NOEFFECT if already acquired;
    // DIERR_INVALIDPARAM before SetDataFormat. Unacquire stops; DI_NOEFFECT if not acquired.
    HResult Acquire();
    HResult Unacquire();

    // The keyboard now, `size` bytes laid out by the data format: 0x80 at a key's offset while it
    // is down. DIERR_INVALIDPARAM for a null buffer or a size other than the format's;
    // DIERR_NOTACQUIRED when not acquired; DIERR_INPUTLOST if Input cannot be read.
    HResult GetDeviceState(std::uint32_t size, std::uint8_t *data);

    // Up to *inOut buffered records, oldest first, into `records` (nullptr: count or discard
    // only); *inOut becomes the number returned. With kGetDataPeek they stay in the buffer.
    // DI_BUFFEROVERFLOW instead of DI_OK when records were lost since the last read that removed
    // records. `elementSize` is the caller's sizeof(DIDEVICEOBJECTDATA), 16 or 20.
    // DIERR_INVALIDPARAM for a null count, another size or unknown flags; DIERR_NOTACQUIRED;
    // DIERR_NOTBUFFERED when the buffer size is 0.
    HResult GetDeviceData(std::uint32_t elementSize, DeviceObjectData *records,
                          std::uint32_t *inOut, std::uint32_t flags);

    // Accepts a keyboard format (c_dfDIKeyboard and any other whose objects are keys): sizes 24
    // and 16, a data size that is a multiple of 4, each object a key (null or GUID_Key, a button
    // type) by DIK_ instance or any instance, inside the data. An object no key matches is
    // skipped if optional, else DIERR_INVALIDPARAM. DIERR_ACQUIRED while acquired.
    HResult SetDataFormat(const DataFormat *format);

    // The event to signal (Threads::SetEvent) whenever an acquired keyboard changes; std::nullopt
    // stops it. DIERR_ACQUIRED while acquired.
    HResult SetEventNotification(std::optional<platform::Waitable> event);

    // One of exclusive and non-exclusive with one of foreground and background, and optionally
    // DISCL_NOWINKEY; a foreground level needs a window. Exclusive background is
    // DIERR_UNSUPPORTED for a keyboard; anything else malformed DIERR_INVALIDPARAM.
    HResult SetCooperativeLevel(bool hasWindow, std::uint32_t flags);

    // The key named by `obj`: DIPH_BYOFFSET an offset in the data format, DIPH_BYID a type with
    // the DIK_ instance. DIERR_OBJECTNOTFOUND for none; DIERR_INVALIDPARAM for a null, a wrong
    // size or another `how`.
    HResult GetObjectInfo(std::uint32_t size, ObjectInstance *info, std::uint32_t obj,
                          std::uint32_t how);

    // The system keyboard's DIDEVICEINSTANCEA. DIERR_INVALIDPARAM for a null or a wrong size.
    HResult GetDeviceInfo(std::uint32_t size, DeviceInstance *info);

    // There is no control panel to show: DI_OK, as DirectInput does when it has none.
    HResult RunControlPanel(std::uint32_t flags);

    // The device is initialised at creation: DI_OK for a supported version and the keyboard's
    // GUID; the version errors of DirectInputCreateA; DIERR_DEVICENOTREG for another device.
    HResult Initialize(std::uint32_t version, const GUID *device);

    // From KeyboardHub::pumpLocked(), with the hub's lock held.
    void onKeyLocked(std::uint8_t dik, bool down, std::uint32_t timeMs, std::uint32_t sequence);
    void onOverflowLocked();
    // After the drain: every key this device reported down that `keyboard` now has up, and for
    // which no release event came (Input queues none when the window loses the focus), gets a
    // release record at `timeMs`, numbered from `sequence` upwards, so the buffer agrees with
    // GetDeviceState. `sequence` is the last number used.
    void releaseHeldLocked(const platform::KeyboardState &keyboard, std::uint32_t timeMs,
                           std::uint32_t &sequence);
    void finishPumpLocked();

private:
    void record(std::uint8_t dik, bool down, std::uint32_t timeMs, std::uint32_t sequence);

    ~KeyboardDevice();

    ObjectInstance describe(std::uint8_t dik) const;

    std::shared_ptr<KeyboardHub> hub_;
    std::uint32_t refs_ = 1;

    bool haveFormat_ = false;
    std::uint32_t dataSize_ = 0;
    // The data format's offset of each DIK_ code, or -1 when the format has no place for it.
    std::array<std::int32_t, 256> offsetOfDik_{};

    bool acquired_ = false;
    std::uint32_t bufferSize_ = 0;
    std::deque<DeviceObjectData> buffer_;
    // Keys reported down and not yet reported up; and keys whose release was made up, so the
    // real release event that may still follow is not reported a second time.
    std::array<bool, 256> down_{};
    std::array<bool, 256> madeUpRelease_{};
    bool overflow_ = false;
    bool changed_ = false;
    std::optional<platform::Waitable> notify_;
};

} // namespace sfcop::hle::dinput
