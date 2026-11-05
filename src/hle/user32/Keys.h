// The keyboard as USER32 shows it: the components' USB HID usage IDs mapped to Win32 virtual
// keys and set-1 scan codes, and the characters a US keyboard types (what TranslateMessage,
// MapVirtualKeyA and VkKeyScanA answer). The tables are written by hand.
#pragma once

#include "platform/Input/Input.h"

#include <cstdint>
#include <optional>

namespace sfcop::hle::user32 {

struct KeyCode {
    std::uint8_t vk = 0;       // the virtual key a message carries (VK_SHIFT, not VK_LSHIFT)
    std::uint8_t sideVk = 0;   // the left/right virtual key for GetAsyncKeyState, or vk
    std::uint8_t scan = 0;     // set-1 scan code
    bool extended = false;     // an E0-prefixed key
};

// The key a HID usage ID is, or nothing for a usage this table does not list.
std::optional<KeyCode> keyFromUsage(platform::Scancode usage);

// The HID usage IDs whose state a virtual key reads (both sides for VK_SHIFT, VK_CONTROL,
// VK_MENU); `count` gets how many, at most 2.
int usagesOfVk(std::uint32_t vk, platform::Scancode out[2]);

// What WM_KEYDOWN's lParam carries for a key: repeat count 1, the scan code, the extended bit,
// and for a key-up the previous-state and transition bits.
std::uint32_t keyLParam(const KeyCode &key, bool down, bool repeat);

// The character a key types with the given modifiers on a US layout, or 0 for none.
char charFromVk(std::uint32_t vk, bool shift, bool control, bool capsLock);

// MapVirtualKeyA: 0 VK to scan code, 1 scan code to VK, 2 VK to its unshifted character (letters
// upper case), 3 scan code to the left/right VK. 0 when there is none.
std::uint32_t mapVirtualKey(std::uint32_t code, std::uint32_t mapType);

// VkKeyScanA: the VK in the low byte and the shift state in the high (1 Shift, 2 Ctrl), or
// 0xFFFF when no key types the character.
std::uint32_t vkKeyScan(char c);

} // namespace sfcop::hle::user32
