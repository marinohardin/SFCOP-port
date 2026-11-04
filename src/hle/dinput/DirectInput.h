// IDirectInputA (DESIGN §5.7, §6.1): DirectInputCreateA's object, serving the system keyboard on
// the Input component. The game creates it with version 0x0700 and asks only for the keyboard
// (src/hle/surface.tsv); any other device is DIERR_DEVICENOTREG, as for a device not installed.
//
// Each method is the COM method of the same name with the guest's pointers already read (a null
// pointer arrives as nullptr); it returns the HRESULT the game reads.
#pragma once

#include "hle/dinput/DInputDefs.h"
#include "hle/dinput/KeyboardDevice.h"
#include "hle/dinput/KeyboardHub.h"

#include <functional>
#include <memory>

namespace sfcop::hle::dinput {

class DirectInput {
public:
    // DirectInputCreateA: a new object with a count of 1 in *out (nullptr on failure). The
    // version errors of checkVersion(); DIERR_INVALIDPARAM for a null `out`;
    // DIERR_NOAGGREGATION when the caller passed an outer IUnknown.
    static HResult Create(std::uint32_t version, bool hasOuter, platform::Input &input,
                          platform::Threads &threads, DirectInput **out);

    DirectInput(const DirectInput &) = delete;
    DirectInput &operator=(const DirectInput &) = delete;

    // COM lifetime: Create() makes the object with `new`; the Release() that brings the count to
    // 0 deletes it. Devices it created keep what they share with it and outlive it.
    std::uint32_t AddRef();
    std::uint32_t Release();

    // A keyboard device with a count of 1 in *out for GUID_SysKeyboard; *out is nullptr on
    // failure. DIERR_INVALIDPARAM for a null `out` or `device`; DIERR_NOAGGREGATION;
    // DIERR_DEVICENOTREG for any other device.
    HResult CreateDevice(const GUID *device, KeyboardDevice **out, bool hasOuter);

    // Calls `callback` with the system keyboard when `devType`'s low byte is 0 (all) or
    // DIDEVTYPE_KEYBOARD, unless `flags` asks for force-feedback devices. DIERR_INVALIDPARAM for
    // an empty callback.
    HResult EnumDevices(std::uint32_t devType,
                        const std::function<bool(const DeviceInstance &)> &callback,
                        std::uint32_t flags);

    // DI_OK for the system keyboard (always attached); DIERR_DEVICENOTREG for another device;
    // DIERR_INVALIDPARAM for a null GUID.
    HResult GetDeviceStatus(const GUID *device);

    // There is no control panel to show: DI_OK.
    HResult RunControlPanel(std::uint32_t flags);

    // The object is initialised at creation: the result of checkVersion().
    HResult Initialize(std::uint32_t version);

    // Moves key events from Input into this object's acquired devices and signals their events.
    // Device reads do this themselves; the runtime calls it after each Window::PumpEvents() so
    // that a notification event is signalled without the game reading first.
    void Pump();

private:
    explicit DirectInput(std::shared_ptr<KeyboardHub> hub) : hub_(std::move(hub)) {}
    ~DirectInput() = default;

    std::shared_ptr<KeyboardHub> hub_;
    std::uint32_t refs_ = 1;
};

} // namespace sfcop::hle::dinput
