// The DirectInput values and structures the module speaks (DESIGN §5.7, §6.1): result codes,
// flags, GUIDs and host forms of the records the game reads. The numbers are the public
// DirectInput definitions; the structures are this module's own, and the guest shims
// (DinputGuest.c) write them into the game's layouts field by field.
#pragma once

#include "hle/com/ComBase.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace sfcop::hle::dinput {

using com::GUID;
// An HRESULT as the game reads it from eax.
using HResult = std::uint32_t;

constexpr HResult kDiOk = 0x00000000;
constexpr HResult kDiNoEffect = 0x00000001;       // S_FALSE
constexpr HResult kDiBufferOverflow = 0x00000001; // S_FALSE
constexpr HResult kDiErrInvalidParam = 0x80070057;
constexpr HResult kDiErrNotAcquired = 0x8007000C;
constexpr HResult kDiErrInputLost = 0x8007001E;
constexpr HResult kDiErrAcquired = 0x800700AA;
constexpr HResult kDiErrOldDirectInputVersion = 0x8007047E;
constexpr HResult kDiErrBetaDirectInputVersion = 0x80070481;
constexpr HResult kDiErrObjectNotFound = 0x80070002;
constexpr HResult kDiErrDeviceNotReg = 0x80040154;
constexpr HResult kDiErrNoAggregation = 0x80040110;
constexpr HResult kDiErrNoInterface = 0x80004002;
constexpr HResult kDiErrUnsupported = 0x80004001;
constexpr HResult kDiErrNotBuffered = 0x80040207;

// The interface versions DirectInputCreateA accepts: DirectX 3 to 7. The game passes 0x0700.
constexpr std::uint32_t kMinVersion = 0x0300;
constexpr std::uint32_t kMaxVersion = 0x0700;

constexpr std::uint32_t kDevTypeKeyboard = 3;         // DIDEVTYPE_KEYBOARD
constexpr std::uint32_t kDevSubtypeKeyboardPcEnh = 4; // DIDEVTYPEKEYBOARD_PCENH
// dwDevType of the system keyboard: type in the low byte, subtype in the next.
constexpr std::uint32_t kKeyboardDevType = kDevTypeKeyboard | kDevSubtypeKeyboardPcEnh << 8;
constexpr std::uint32_t kEnumFlagForceFeedback = 0x00000100; // DIEDFL_FORCEFEEDBACK
constexpr std::uint32_t kDeviceAttached = 0x00000001;        // DIDC_ATTACHED

constexpr std::uint32_t kCoopExclusive = 0x01;    // DISCL_EXCLUSIVE
constexpr std::uint32_t kCoopNonExclusive = 0x02; // DISCL_NONEXCLUSIVE
constexpr std::uint32_t kCoopForeground = 0x04;   // DISCL_FOREGROUND
constexpr std::uint32_t kCoopBackground = 0x08;   // DISCL_BACKGROUND
constexpr std::uint32_t kCoopNoWinKey = 0x10;     // DISCL_NOWINKEY

constexpr std::uint32_t kGetDataPeek = 0x00000001; // DIGDD_PEEK
constexpr std::uint32_t kInfinite = 0xFFFFFFFF;    // a GetDeviceData count meaning "all"

constexpr std::uint32_t kPropBufferSize = 1;  // DIPROP_BUFFERSIZE: MAKEDIPROP(1)
constexpr std::uint32_t kPropHowDevice = 0;   // DIPH_DEVICE
constexpr std::uint32_t kPropHowByOffset = 1; // DIPH_BYOFFSET
constexpr std::uint32_t kPropHowById = 2;     // DIPH_BYID
constexpr std::uint32_t kPropDwordSize = 20;  // sizeof(DIPROPDWORD)
constexpr std::uint32_t kPropHeaderSize = 16; // sizeof(DIPROPHEADER)

constexpr std::uint32_t kTypeAll = 0x00000000;        // DIDFT_ALL
constexpr std::uint32_t kTypePushButton = 0x00000004; // DIDFT_PSHBUTTON
constexpr std::uint32_t kTypeButton = 0x0000000C;     // DIDFT_BUTTON: push or toggle
constexpr std::uint32_t kTypeAnyInstance = 0x00FFFF00;
constexpr std::uint32_t kTypeOptional = 0x80000000; // DIDFT_OPTIONAL
constexpr std::uint32_t typeInstance(std::uint32_t type) {
    return (type >> 8) & 0xFFFF;
}
constexpr std::uint32_t makeInstance(std::uint32_t instance) {
    return (instance & 0xFFFF) << 8;
}

constexpr bool kEnumStop = false; // DIENUM_STOP: a callback returning it ends an enumeration

// Structure sizes the game's dwSize fields are checked against: the DirectX 3 form and the
// DirectX 5 to 7 form of each.
constexpr std::uint32_t kDevCapsSizeDx3 = 24;
constexpr std::uint32_t kDevCapsSize = 44;
constexpr std::uint32_t kDeviceInstanceSizeDx3 = 560;
constexpr std::uint32_t kDeviceInstanceSize = 580;
constexpr std::uint32_t kObjectInstanceSizeDx3 = 292;
constexpr std::uint32_t kObjectInstanceSize = 316;
constexpr std::uint32_t kDataFormatSize = 24;   // sizeof(DIDATAFORMAT)
constexpr std::uint32_t kObjectFormatSize = 16; // sizeof(DIOBJECTDATAFORMAT)
constexpr std::uint32_t kObjectDataSizeDx3 = 16;
constexpr std::uint32_t kObjectDataSize = 20; // with uAppData, written as 0

// {6F1D2B61-D5A0-11CF-BFC7-444553540000}
constexpr GUID kGuidSysKeyboard = {
    0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
// {55728220-D33C-11CF-BFC7-444553540000}
constexpr GUID kGuidKey = {
    0x55728220, 0xD33C, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

inline bool sameGuid(const GUID &a, const GUID &b) {
    return a.Data1 == b.Data1 && a.Data2 == b.Data2 && a.Data3 == b.Data3 &&
           std::memcmp(a.Data4, b.Data4, sizeof a.Data4) == 0;
}

// One buffered record (DIDEVICEOBJECTDATA): the key's offset in the data format, 0x80 pressed or
// 0 released, the time on the GetTickCount scale and a sequence number that increases.
struct DeviceObjectData {
    std::uint32_t ofs = 0;
    std::uint32_t data = 0;
    std::uint32_t timeStamp = 0;
    std::uint32_t sequence = 0;
};

// One DIOBJECTDATAFORMAT. `guid` is empty when the game passed a null pguid (any type).
struct ObjectFormat {
    std::optional<GUID> guid;
    std::uint32_t ofs = 0;
    std::uint32_t type = 0;
    std::uint32_t flags = 0;
};

// A DIDATAFORMAT with its object array read out.
struct DataFormat {
    std::uint32_t size = 0;
    std::uint32_t objSize = 0;
    std::uint32_t flags = 0;
    std::uint32_t dataSize = 0;
    std::vector<ObjectFormat> objects;
};

// DIDEVICEINSTANCEA: what EnumDevices and GetDeviceInfo report.
struct DeviceInstance {
    GUID instance{};
    GUID product{};
    std::uint32_t devType = 0;
    std::string instanceName;
    std::string productName;
};

// DIDEVICEOBJECTINSTANCEA: what EnumObjects and GetObjectInfo report for one key.
struct ObjectInstance {
    GUID guidType{};
    std::uint32_t ofs = 0;
    std::uint32_t type = 0;
    std::uint32_t flags = 0;
    std::string name;
};

// DIDEVCAPS, as far as a keyboard fills it.
struct DeviceCaps {
    std::uint32_t flags = 0;
    std::uint32_t devType = 0;
    std::uint32_t axes = 0;
    std::uint32_t buttons = 0;
    std::uint32_t povs = 0;
};

// Whether this module serves an interface version: DI_OK for DirectX 3 to 7; a newer version is
// one this DirectInput is too old for, an older one a pre-release.
inline HResult checkVersion(std::uint32_t version) {
    if (version > kMaxVersion) {
        return kDiErrOldDirectInputVersion;
    }
    if (version < kMinVersion) {
        return kDiErrBetaDirectInputVersion;
    }
    return kDiOk;
}

// The one device served: the system keyboard, as EnumDevices and GetDeviceInfo report it.
inline DeviceInstance systemKeyboard() {
    DeviceInstance keyboard;
    keyboard.instance = kGuidSysKeyboard;
    keyboard.product = kGuidSysKeyboard;
    keyboard.devType = kKeyboardDevType;
    keyboard.instanceName = "Keyboard";
    keyboard.productName = "Keyboard";
    return keyboard;
}

} // namespace sfcop::hle::dinput
