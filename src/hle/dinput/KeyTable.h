// The keyboard's keys as DirectInput names them: each USB HID usage ID (the key identity of
// platform/Input/Input.h) with its DIK_ code and a display name. DIK_ codes are the PC scan code
// set 1, extended (E0-prefixed) keys with bit 7 set.
#pragma once

#include "platform/Input/Input.h"

#include <cstddef>
#include <cstdint>

namespace sfcop::hle::dinput {

struct KeyInfo {
    platform::Scancode usage; // USB HID keyboard usage ID
    std::uint8_t dik;         // DIK_ code, never 0
    const char *name;
};

// Every key the keyboard reports, in ascending DIK_ order; no two share a DIK_ code.
const KeyInfo *keyTable(std::size_t *count);

// The DIK_ code of a usage, or 0 if DirectInput has none for it.
std::uint8_t dikFromUsage(platform::Scancode usage);

// The key with this DIK_ code, or nullptr.
const KeyInfo *keyFromDik(std::uint32_t dik);

} // namespace sfcop::hle::dinput
