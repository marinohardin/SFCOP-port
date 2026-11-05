#include "app/input/VirtualKeys.h"

namespace sfcop::app::input {

namespace {

struct KeyRow {
    std::uint16_t virtualKey;
    platform::Scancode usage;
};

// Windows VK_* value -> USB HID usage (page 0x07).
constexpr KeyRow kKeys[] = {
    {0x08, 0x2A}, // Backspace
    {0x09, 0x2B}, // Tab
    {0x0D, 0x28}, // Return
    {0x10, 0xE1}, // Shift (left)
    {0x11, 0xE0}, // Control (left)
    {0x12, 0xE2}, // Alt (left)
    {0x13, 0x48}, // Pause
    {0x14, 0x39}, // Caps Lock
    {0x1B, 0x29}, // Escape
    {0x20, 0x2C}, // Space
    {0x21, 0x4B}, // Page Up
    {0x22, 0x4E}, // Page Down
    {0x23, 0x4D}, // End
    {0x24, 0x4A}, // Home
    {0x25, 0x50}, // Left
    {0x26, 0x52}, // Up
    {0x27, 0x4F}, // Right
    {0x28, 0x51}, // Down
    {0x2C, 0x46}, // Print Screen
    {0x2D, 0x49}, // Insert
    {0x2E, 0x4C}, // Delete
    {0x30, 0x27}, // 0
    {0x60, 0x62}, // keypad 0
    {0x6A, 0x55}, // keypad *
    {0x6B, 0x57}, // keypad +
    {0x6D, 0x56}, // keypad -
    {0x6E, 0x63}, // keypad .
    {0x6F, 0x54}, // keypad /
    {0x90, 0x53}, // Num Lock
    {0x91, 0x47}, // Scroll Lock
    {0xA0, 0xE1}, // left Shift
    {0xA1, 0xE5}, // right Shift
    {0xA2, 0xE0}, // left Control
    {0xA3, 0xE4}, // right Control
    {0xA4, 0xE2}, // left Alt
    {0xA5, 0xE6}, // right Alt
    {0xBA, 0x33}, // ;
    {0xBB, 0x2E}, // =
    {0xBC, 0x36}, // ,
    {0xBD, 0x2D}, // -
    {0xBE, 0x37}, // .
    {0xBF, 0x38}, // /
    {0xC0, 0x35}, // `
    {0xDB, 0x2F}, // [
    {0xDC, 0x31}, // backslash
    {0xDD, 0x30}, // ]
    {0xDE, 0x34}, // '
};

} // namespace

std::optional<platform::Scancode> scancodeForVirtualKey(std::uint16_t virtualKey) {
    // The runs: 1-9, A-Z, keypad 1-9, F1-F12.
    if (virtualKey >= 0x31 && virtualKey <= 0x39) {
        return static_cast<platform::Scancode>(0x1E + (virtualKey - 0x31));
    }
    if (virtualKey >= 0x41 && virtualKey <= 0x5A) {
        return static_cast<platform::Scancode>(0x04 + (virtualKey - 0x41));
    }
    if (virtualKey >= 0x61 && virtualKey <= 0x69) {
        return static_cast<platform::Scancode>(0x59 + (virtualKey - 0x61));
    }
    if (virtualKey >= 0x70 && virtualKey <= 0x7B) {
        return static_cast<platform::Scancode>(0x3A + (virtualKey - 0x70));
    }
    for (const KeyRow &row : kKeys) {
        if (row.virtualKey == virtualKey) {
            return row.usage;
        }
    }
    return std::nullopt;
}

} // namespace sfcop::app::input
