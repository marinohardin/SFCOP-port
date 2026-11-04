// Shared by the DirectInput module's tests: the game's keyboard data format and event builders.
#pragma once

#include "hle/dinput/DInputDefs.h"
#include "hle/dinput/KeyboardDevice.h"
#include "platform/Input/Input.h"

namespace sfcop::hle::dinput::test {

// c_dfDIKeyboard as the game links it: 256 optional button objects, instance i at offset i.
inline DataFormat keyboardFormat() {
    DataFormat format;
    format.size = 24;
    format.objSize = 16;
    format.flags = 2; // DIDF_RELAXIS
    format.dataSize = 256;
    for (std::uint32_t i = 0; i < 256; ++i) {
        ObjectFormat object;
        object.guid = kGuidKey;
        object.ofs = i;
        object.type = 0x80000000 | 0x0C | (i << 8);
        format.objects.push_back(object);
    }
    return format;
}

inline platform::InputEvent keyEvent(bool down, platform::Scancode usage, std::uint32_t timeMs,
                                     bool repeat = false) {
    platform::InputEvent event;
    event.type = down ? platform::InputEventType::kKeyDown : platform::InputEventType::kKeyUp;
    event.key = usage;
    event.timeMs = timeMs;
    event.repeat = repeat;
    return event;
}

inline platform::InputEvent mouseMoveEvent(std::uint32_t timeMs) {
    platform::InputEvent event;
    event.type = platform::InputEventType::kMouseMove;
    event.timeMs = timeMs;
    event.dx = 3;
    return event;
}

// A DIPROPDWORD header on the device.
inline PropertyHeader deviceHeader() {
    return PropertyHeader{20, 16, 0, 0};
}

} // namespace sfcop::hle::dinput::test
