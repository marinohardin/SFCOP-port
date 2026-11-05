// The Windows virtual keys a steps file names (`key <vk>`), as the USB HID keyboard usage IDs the
// Input contract uses (platform/Input/Input.h). The table is written by hand for a US layout; a
// left/right-neutral key (VK_SHIFT, VK_CONTROL, VK_MENU) is its left-hand key.
#pragma once

#include "platform/Input/Input.h"

#include <cstdint>
#include <optional>

namespace sfcop::app::input {

// The usage ID for `virtualKey`, or nothing when the table has no entry for it.
std::optional<platform::Scancode> scancodeForVirtualKey(std::uint16_t virtualKey);

} // namespace sfcop::app::input
