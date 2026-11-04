#include "hle/dinput/KeyboardDevice.h"

#include "hle/dinput/KeyTable.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace sfcop::hle::dinput {
namespace {

constexpr std::uint32_t kAnyInstance = 0xFFFF;
constexpr std::uint32_t kKnownCoopFlags =
    kCoopExclusive | kCoopNonExclusive | kCoopForeground | kCoopBackground | kCoopNoWinKey;

bool validDwordHeader(const PropertyHeader &header) {
    return header.size == kPropDwordSize && header.headerSize == kPropHeaderSize &&
           header.how == kPropHowDevice && header.obj == 0;
}

// Whether a DIOBJECTDATAFORMAT entry asks for a key: no GUID or GUID_Key, and a button type
// (or any type).
bool asksForKey(const ObjectFormat &object) {
    bool keyGuid = !object.guid || sameGuid(*object.guid, kGuidKey);
    bool buttonType = (object.type & 0xFF) == kTypeAll || (object.type & kTypeButton) != 0;
    return keyGuid && buttonType;
}

} // namespace

KeyboardDevice::KeyboardDevice(std::shared_ptr<KeyboardHub> hub) : hub_(std::move(hub)) {
    offsetOfDik_.fill(-1);
    std::lock_guard<std::mutex> lock(hub_->mutex());
    hub_->attach(this);
}

KeyboardDevice::~KeyboardDevice() {
    std::lock_guard<std::mutex> lock(hub_->mutex());
    hub_->detach(this);
}

std::uint32_t KeyboardDevice::AddRef() {
    std::lock_guard<std::mutex> lock(hub_->mutex());
    return ++refs_;
}

std::uint32_t KeyboardDevice::Release() {
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

HResult KeyboardDevice::GetCapabilities(std::uint32_t size, DeviceCaps *caps) {
    if (caps == nullptr || (size != kDevCapsSizeDx3 && size != kDevCapsSize)) {
        return kDiErrInvalidParam;
    }
    std::size_t keys = 0;
    keyTable(&keys);
    *caps = DeviceCaps{};
    caps->flags = kDeviceAttached;
    caps->devType = kKeyboardDevType;
    caps->buttons = static_cast<std::uint32_t>(keys);
    return kDiOk;
}

ObjectInstance KeyboardDevice::describe(std::uint8_t dik) const {
    ObjectInstance object;
    object.guidType = kGuidKey;
    object.ofs =
        haveFormat_ && offsetOfDik_[dik] >= 0 ? static_cast<std::uint32_t>(offsetOfDik_[dik]) : dik;
    object.type = kTypePushButton | makeInstance(dik);
    object.name = keyFromDik(dik)->name;
    return object;
}

HResult KeyboardDevice::EnumObjects(const std::function<bool(const ObjectInstance &)> &callback,
                                    std::uint32_t flags) {
    if (!callback) {
        return kDiErrInvalidParam;
    }
    if ((flags & 0xFF) != kTypeAll && (flags & kTypeButton) == 0) {
        return kDiOk; // a keyboard has no axes, POVs or collections
    }
    std::vector<ObjectInstance> objects;
    {
        std::lock_guard<std::mutex> lock(hub_->mutex());
        std::size_t count = 0;
        const KeyInfo *keys = keyTable(&count);
        for (std::size_t i = 0; i < count; ++i) {
            objects.push_back(describe(keys[i].dik));
        }
    }
    // The callback is the game's code: no lock is held while it runs.
    for (const ObjectInstance &object : objects) {
        if (callback(object) == kEnumStop) {
            break;
        }
    }
    return kDiOk;
}

HResult KeyboardDevice::GetProperty(std::uint32_t property, const PropertyHeader &header,
                                    std::uint32_t *value) {
    if (property != kPropBufferSize) {
        return kDiErrUnsupported;
    }
    if (value == nullptr || !validDwordHeader(header)) {
        return kDiErrInvalidParam;
    }
    std::lock_guard<std::mutex> lock(hub_->mutex());
    *value = bufferSize_;
    return kDiOk;
}

HResult KeyboardDevice::SetProperty(std::uint32_t property, const PropertyHeader &header,
                                    std::uint32_t value) {
    if (property != kPropBufferSize) {
        return kDiErrUnsupported;
    }
    if (!validDwordHeader(header)) {
        return kDiErrInvalidParam;
    }
    std::lock_guard<std::mutex> lock(hub_->mutex());
    if (acquired_) {
        return kDiErrAcquired;
    }
    bufferSize_ = value;
    buffer_.clear();
    overflow_ = false;
    return kDiOk;
}

HResult KeyboardDevice::Acquire() {
    std::lock_guard<std::mutex> lock(hub_->mutex());
    if (!haveFormat_) {
        return kDiErrInvalidParam;
    }
    if (acquired_) {
        return kDiNoEffect;
    }
    // What happened before acquisition is not this device's data: let it pass while unacquired.
    hub_->pumpLocked();
    buffer_.clear();
    overflow_ = false;
    down_.fill(false);
    madeUpRelease_.fill(false);
    acquired_ = true;
    return kDiOk;
}

HResult KeyboardDevice::Unacquire() {
    std::lock_guard<std::mutex> lock(hub_->mutex());
    if (!acquired_) {
        return kDiNoEffect;
    }
    acquired_ = false;
    return kDiOk;
}

HResult KeyboardDevice::GetDeviceState(std::uint32_t size, std::uint8_t *data) {
    std::lock_guard<std::mutex> lock(hub_->mutex());
    if (data == nullptr) {
        return kDiErrInvalidParam;
    }
    if (!acquired_) {
        return kDiErrNotAcquired;
    }
    if (size != dataSize_) {
        return kDiErrInvalidParam;
    }
    hub_->pumpLocked();
    platform::Result<platform::KeyboardState> keyboard = hub_->input().GetKeyboard();
    if (!keyboard.Ok()) {
        return kDiErrInputLost;
    }
    std::memset(data, 0, size);
    for (std::size_t usage = 0; usage < platform::kScancodeCount; ++usage) {
        if (!keyboard.Value()[usage]) {
            continue;
        }
        std::uint8_t dik = dikFromUsage(static_cast<platform::Scancode>(usage));
        if (dik != 0 && offsetOfDik_[dik] >= 0) {
            data[offsetOfDik_[dik]] = 0x80;
        }
    }
    return kDiOk;
}

HResult KeyboardDevice::GetDeviceData(std::uint32_t elementSize, DeviceObjectData *records,
                                      std::uint32_t *inOut, std::uint32_t flags) {
    if (inOut == nullptr || (elementSize != kObjectDataSizeDx3 && elementSize != kObjectDataSize) ||
        (flags & ~kGetDataPeek) != 0) {
        return kDiErrInvalidParam;
    }
    std::lock_guard<std::mutex> lock(hub_->mutex());
    if (!acquired_) {
        return kDiErrNotAcquired;
    }
    if (bufferSize_ == 0) {
        return kDiErrNotBuffered;
    }
    hub_->pumpLocked();
    std::size_t count = (std::min)(static_cast<std::size_t>(*inOut), buffer_.size());
    if (records != nullptr) {
        std::copy_n(buffer_.begin(), count, records);
    }
    HResult result = overflow_ ? kDiBufferOverflow : kDiOk;
    if ((flags & kGetDataPeek) == 0) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(count));
        overflow_ = false;
    }
    *inOut = static_cast<std::uint32_t>(count);
    return result;
}

HResult KeyboardDevice::SetDataFormat(const DataFormat *format) {
    if (format == nullptr) {
        return kDiErrInvalidParam;
    }
    if (format->size != kDataFormatSize || format->objSize != kObjectFormatSize ||
        format->dataSize == 0 || format->dataSize % 4 != 0) {
        return kDiErrInvalidParam;
    }
    std::array<std::int32_t, 256> offsets;
    offsets.fill(-1);
    std::size_t count = 0;
    const KeyInfo *keys = keyTable(&count);
    for (const ObjectFormat &object : format->objects) {
        if (object.ofs >= format->dataSize) {
            return kDiErrInvalidParam;
        }
        const KeyInfo *match = nullptr;
        if (asksForKey(object)) {
            std::uint32_t instance = typeInstance(object.type);
            if (instance == kAnyInstance) {
                for (std::size_t i = 0; i < count && match == nullptr; ++i) {
                    if (offsets[keys[i].dik] < 0) {
                        match = &keys[i];
                    }
                }
            } else if (const KeyInfo *key = keyFromDik(instance);
                       key != nullptr && offsets[key->dik] < 0) {
                match = key;
            }
        }
        if (match != nullptr) {
            offsets[match->dik] = static_cast<std::int32_t>(object.ofs);
        } else if ((object.type & kTypeOptional) == 0) {
            return kDiErrInvalidParam;
        }
    }
    std::lock_guard<std::mutex> lock(hub_->mutex());
    if (acquired_) {
        return kDiErrAcquired;
    }
    offsetOfDik_ = offsets;
    dataSize_ = format->dataSize;
    haveFormat_ = true;
    return kDiOk;
}

HResult KeyboardDevice::SetEventNotification(std::optional<platform::Waitable> event) {
    std::lock_guard<std::mutex> lock(hub_->mutex());
    if (acquired_) {
        return kDiErrAcquired;
    }
    notify_ = event;
    return kDiOk;
}

HResult KeyboardDevice::SetCooperativeLevel(bool hasWindow, std::uint32_t flags) {
    bool exclusive = (flags & kCoopExclusive) != 0;
    bool nonExclusive = (flags & kCoopNonExclusive) != 0;
    bool foreground = (flags & kCoopForeground) != 0;
    bool background = (flags & kCoopBackground) != 0;
    if ((flags & ~kKnownCoopFlags) != 0 || exclusive == nonExclusive || foreground == background) {
        return kDiErrInvalidParam;
    }
    if (exclusive && background) {
        return kDiErrUnsupported;
    }
    if (foreground && !hasWindow) {
        return kDiErrInvalidParam;
    }
    // Nothing to apply: the keyboard is never taken from other programs, and the window delivers
    // keys only while it has the focus.
    return kDiOk;
}

HResult KeyboardDevice::GetObjectInfo(std::uint32_t size, ObjectInstance *info, std::uint32_t obj,
                                      std::uint32_t how) {
    if (info == nullptr || (size != kObjectInstanceSizeDx3 && size != kObjectInstanceSize)) {
        return kDiErrInvalidParam;
    }
    std::lock_guard<std::mutex> lock(hub_->mutex());
    if (how == kPropHowByOffset) {
        if (haveFormat_) {
            for (std::uint32_t dik = 0; dik < 256; ++dik) {
                if (offsetOfDik_[dik] >= 0 &&
                    static_cast<std::uint32_t>(offsetOfDik_[dik]) == obj &&
                    keyFromDik(dik) != nullptr) {
                    *info = describe(static_cast<std::uint8_t>(dik));
                    return kDiOk;
                }
            }
        }
        return kDiErrObjectNotFound;
    }
    if (how == kPropHowById) {
        std::uint32_t dik = typeInstance(obj);
        if ((obj & kTypeButton) != 0 && keyFromDik(dik) != nullptr) {
            *info = describe(static_cast<std::uint8_t>(dik));
            return kDiOk;
        }
        return kDiErrObjectNotFound;
    }
    return kDiErrInvalidParam;
}

HResult KeyboardDevice::GetDeviceInfo(std::uint32_t size, DeviceInstance *info) {
    if (info == nullptr || (size != kDeviceInstanceSizeDx3 && size != kDeviceInstanceSize)) {
        return kDiErrInvalidParam;
    }
    *info = systemKeyboard();
    return kDiOk;
}

HResult KeyboardDevice::RunControlPanel(std::uint32_t) {
    return kDiOk;
}

HResult KeyboardDevice::Initialize(std::uint32_t version, const GUID *device) {
    if (HResult result = checkVersion(version); result != kDiOk) {
        return result;
    }
    if (device == nullptr) {
        return kDiErrInvalidParam;
    }
    return sameGuid(*device, kGuidSysKeyboard) ? kDiOk : kDiErrDeviceNotReg;
}

void KeyboardDevice::onKeyLocked(std::uint8_t dik, bool down, std::uint32_t timeMs,
                                 std::uint32_t sequence) {
    if (!acquired_) {
        return;
    }
    if (madeUpRelease_[dik]) {
        madeUpRelease_[dik] = false;
        if (!down) {
            return; // the release that was already reported
        }
    }
    down_[dik] = down;
    changed_ = true;
    record(dik, down, timeMs, sequence);
}

void KeyboardDevice::record(std::uint8_t dik, bool down, std::uint32_t timeMs,
                            std::uint32_t sequence) {
    if (bufferSize_ == 0 || offsetOfDik_[dik] < 0) {
        return;
    }
    if (buffer_.size() >= bufferSize_) {
        overflow_ = true;
        return;
    }
    DeviceObjectData record;
    record.ofs = static_cast<std::uint32_t>(offsetOfDik_[dik]);
    record.data = down ? 0x80 : 0x00;
    record.timeStamp = timeMs;
    record.sequence = sequence;
    buffer_.push_back(record);
}

void KeyboardDevice::releaseHeldLocked(const platform::KeyboardState &keyboard,
                                       std::uint32_t timeMs, std::uint32_t &sequence) {
    if (!acquired_) {
        return;
    }
    for (std::size_t usage = 0; usage < platform::kScancodeCount; ++usage) {
        std::uint8_t dik = dikFromUsage(static_cast<platform::Scancode>(usage));
        if (dik == 0 || !down_[dik] || keyboard[usage]) {
            continue;
        }
        down_[dik] = false;
        madeUpRelease_[dik] = true;
        changed_ = true;
        record(dik, false, timeMs, ++sequence);
    }
}

void KeyboardDevice::onOverflowLocked() {
    if (acquired_ && bufferSize_ != 0) {
        overflow_ = true;
    }
}

void KeyboardDevice::finishPumpLocked() {
    if (changed_ && notify_) {
        // A failure to signal leaves the game polling, which it does every frame anyway.
        (void)hub_->threads().SetEvent(*notify_);
    }
    changed_ = false;
}

} // namespace sfcop::hle::dinput
