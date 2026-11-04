#include "hle/dinput/DirectInput.h"

namespace sfcop::hle::dinput {

HResult DirectInput::Create(std::uint32_t version, bool hasOuter, platform::Input &input,
                            platform::Threads &threads, DirectInput **out) {
    if (out == nullptr) {
        return kDiErrInvalidParam;
    }
    *out = nullptr;
    if (HResult result = checkVersion(version); result != kDiOk) {
        return result;
    }
    if (hasOuter) {
        return kDiErrNoAggregation;
    }
    *out = new DirectInput(std::make_shared<KeyboardHub>(input, threads)); // a COM object
    return kDiOk;
}

std::uint32_t DirectInput::AddRef() {
    std::lock_guard<std::mutex> lock(hub_->mutex());
    return ++refs_;
}

std::uint32_t DirectInput::Release() {
    std::uint32_t left;
    {
        std::lock_guard<std::mutex> lock(hub_->mutex());
        left = --refs_;
    }
    if (left == 0) {
        delete this; // the COM object's last reference
    }
    return left;
}

HResult DirectInput::CreateDevice(const GUID *device, KeyboardDevice **out, bool hasOuter) {
    if (out == nullptr) {
        return kDiErrInvalidParam;
    }
    *out = nullptr;
    if (device == nullptr) {
        return kDiErrInvalidParam;
    }
    if (hasOuter) {
        return kDiErrNoAggregation;
    }
    if (!sameGuid(*device, kGuidSysKeyboard)) {
        return kDiErrDeviceNotReg;
    }
    *out = new KeyboardDevice(hub_); // a COM object
    return kDiOk;
}

HResult DirectInput::EnumDevices(std::uint32_t devType,
                                 const std::function<bool(const DeviceInstance &)> &callback,
                                 std::uint32_t flags) {
    if (!callback) {
        return kDiErrInvalidParam;
    }
    std::uint32_t type = devType & 0xFF;
    if ((type == 0 || type == kDevTypeKeyboard) && (flags & kEnumFlagForceFeedback) == 0) {
        callback(systemKeyboard()); // one device: whether to continue does not matter
    }
    return kDiOk;
}

HResult DirectInput::GetDeviceStatus(const GUID *device) {
    if (device == nullptr) {
        return kDiErrInvalidParam;
    }
    return sameGuid(*device, kGuidSysKeyboard) ? kDiOk : kDiErrDeviceNotReg;
}

HResult DirectInput::RunControlPanel(std::uint32_t) {
    return kDiOk;
}

HResult DirectInput::Initialize(std::uint32_t version) {
    return checkVersion(version);
}

void DirectInput::Pump() {
    hub_->pump();
}

} // namespace sfcop::hle::dinput
