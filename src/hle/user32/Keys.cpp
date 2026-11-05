#include "hle/user32/Keys.h"

#include "hle/user32/User32Defs.h"

#include <cctype>
#include <cstring>

namespace sfcop::hle::user32 {

namespace {

struct Row {
    std::uint16_t usage;
    std::uint8_t vk, sideVk, scan;
    bool extended;
};

// HID usage, VK, left/right VK, set-1 scan code, E0 prefix.
constexpr Row kKeys[] = {
    {0x04, 'A', 'A', 0x1E, false}, {0x05, 'B', 'B', 0x30, false}, {0x06, 'C', 'C', 0x2E, false},
    {0x07, 'D', 'D', 0x20, false}, {0x08, 'E', 'E', 0x12, false}, {0x09, 'F', 'F', 0x21, false},
    {0x0A, 'G', 'G', 0x22, false}, {0x0B, 'H', 'H', 0x23, false}, {0x0C, 'I', 'I', 0x17, false},
    {0x0D, 'J', 'J', 0x24, false}, {0x0E, 'K', 'K', 0x25, false}, {0x0F, 'L', 'L', 0x26, false},
    {0x10, 'M', 'M', 0x32, false}, {0x11, 'N', 'N', 0x31, false}, {0x12, 'O', 'O', 0x18, false},
    {0x13, 'P', 'P', 0x19, false}, {0x14, 'Q', 'Q', 0x10, false}, {0x15, 'R', 'R', 0x13, false},
    {0x16, 'S', 'S', 0x1F, false}, {0x17, 'T', 'T', 0x14, false}, {0x18, 'U', 'U', 0x16, false},
    {0x19, 'V', 'V', 0x2F, false}, {0x1A, 'W', 'W', 0x11, false}, {0x1B, 'X', 'X', 0x2D, false},
    {0x1C, 'Y', 'Y', 0x15, false}, {0x1D, 'Z', 'Z', 0x2C, false},
    {0x1E, '1', '1', 0x02, false}, {0x1F, '2', '2', 0x03, false}, {0x20, '3', '3', 0x04, false},
    {0x21, '4', '4', 0x05, false}, {0x22, '5', '5', 0x06, false}, {0x23, '6', '6', 0x07, false},
    {0x24, '7', '7', 0x08, false}, {0x25, '8', '8', 0x09, false}, {0x26, '9', '9', 0x0A, false},
    {0x27, '0', '0', 0x0B, false},
    {0x28, 0x0D, 0x0D, 0x1C, false}, // Return
    {0x29, 0x1B, 0x1B, 0x01, false}, // Escape
    {0x2A, 0x08, 0x08, 0x0E, false}, // Backspace
    {0x2B, 0x09, 0x09, 0x0F, false}, // Tab
    {0x2C, 0x20, 0x20, 0x39, false}, // Space
    {0x2D, 0xBD, 0xBD, 0x0C, false}, // - _
    {0x2E, 0xBB, 0xBB, 0x0D, false}, // = +
    {0x2F, 0xDB, 0xDB, 0x1A, false}, // [ {
    {0x30, 0xDD, 0xDD, 0x1B, false}, // ] }
    {0x31, 0xDC, 0xDC, 0x2B, false}, // \ |
    {0x32, 0xDC, 0xDC, 0x2B, false}, // non-US # ~, at the backslash's place
    {0x33, 0xBA, 0xBA, 0x27, false}, // ; :
    {0x34, 0xDE, 0xDE, 0x28, false}, // ' "
    {0x35, 0xC0, 0xC0, 0x29, false}, // ` ~
    {0x36, 0xBC, 0xBC, 0x33, false}, // , <
    {0x37, 0xBE, 0xBE, 0x34, false}, // . >
    {0x38, 0xBF, 0xBF, 0x35, false}, // / ?
    {0x39, 0x14, 0x14, 0x3A, false}, // Caps Lock
    {0x3A, 0x70, 0x70, 0x3B, false}, {0x3B, 0x71, 0x71, 0x3C, false}, // F1, F2
    {0x3C, 0x72, 0x72, 0x3D, false}, {0x3D, 0x73, 0x73, 0x3E, false}, // F3, F4
    {0x3E, 0x74, 0x74, 0x3F, false}, {0x3F, 0x75, 0x75, 0x40, false}, // F5, F6
    {0x40, 0x76, 0x76, 0x41, false}, {0x41, 0x77, 0x77, 0x42, false}, // F7, F8
    {0x42, 0x78, 0x78, 0x43, false}, {0x43, 0x79, 0x79, 0x44, false}, // F9, F10
    {0x44, 0x7A, 0x7A, 0x57, false}, {0x45, 0x7B, 0x7B, 0x58, false}, // F11, F12
    {0x46, 0x2C, 0x2C, 0x37, true},  // Print Screen
    {0x47, 0x91, 0x91, 0x46, false}, // Scroll Lock
    {0x48, 0x13, 0x13, 0x45, false}, // Pause
    {0x49, 0x2D, 0x2D, 0x52, true},  // Insert
    {0x4A, 0x24, 0x24, 0x47, true},  // Home
    {0x4B, 0x21, 0x21, 0x49, true},  // Page Up
    {0x4C, 0x2E, 0x2E, 0x53, true},  // Delete
    {0x4D, 0x23, 0x23, 0x4F, true},  // End
    {0x4E, 0x22, 0x22, 0x51, true},  // Page Down
    {0x4F, 0x27, 0x27, 0x4D, true},  // Right
    {0x50, 0x25, 0x25, 0x4B, true},  // Left
    {0x51, 0x28, 0x28, 0x50, true},  // Down
    {0x52, 0x26, 0x26, 0x48, true},  // Up
    {0x53, 0x90, 0x90, 0x45, true},  // Num Lock
    {0x54, 0x6F, 0x6F, 0x35, true},  // keypad /
    {0x55, 0x6A, 0x6A, 0x37, false}, // keypad *
    {0x56, 0x6D, 0x6D, 0x4A, false}, // keypad -
    {0x57, 0x6B, 0x6B, 0x4E, false}, // keypad +
    {0x58, 0x0D, 0x0D, 0x1C, true},  // keypad Enter
    {0x59, 0x61, 0x61, 0x4F, false}, {0x5A, 0x62, 0x62, 0x50, false}, // keypad 1, 2
    {0x5B, 0x63, 0x63, 0x51, false}, {0x5C, 0x64, 0x64, 0x4B, false}, // keypad 3, 4
    {0x5D, 0x65, 0x65, 0x4C, false}, {0x5E, 0x66, 0x66, 0x4D, false}, // keypad 5, 6
    {0x5F, 0x67, 0x67, 0x47, false}, {0x60, 0x68, 0x68, 0x48, false}, // keypad 7, 8
    {0x61, 0x69, 0x69, 0x49, false}, {0x62, 0x60, 0x60, 0x52, false}, // keypad 9, 0
    {0x63, 0x6E, 0x6E, 0x53, false}, // keypad .
    {0x64, 0xE2, 0xE2, 0x56, false}, // non-US \ |
    {0x65, 0x5D, 0x5D, 0x5D, true},  // Application
    {0xE0, 0x11, 0xA2, 0x1D, false}, // left Ctrl
    {0xE1, 0x10, 0xA0, 0x2A, false}, // left Shift
    {0xE2, 0x12, 0xA4, 0x38, false}, // left Alt
    {0xE3, 0x5B, 0x5B, 0x5B, true},  // left GUI
    {0xE4, 0x11, 0xA3, 0x1D, true},  // right Ctrl
    {0xE5, 0x10, 0xA1, 0x36, false}, // right Shift
    {0xE6, 0x12, 0xA5, 0x38, true},  // right Alt
    {0xE7, 0x5C, 0x5C, 0x5C, true},  // right GUI
};

constexpr char kShiftedDigits[] = ")!@#$%^&*(";

struct Oem {
    std::uint8_t vk;
    char plain, shifted;
};
constexpr Oem kOem[] = {
    {0xBA, ';', ':'}, {0xBB, '=', '+'}, {0xBC, ',', '<'}, {0xBD, '-', '_'}, {0xBE, '.', '>'},
    {0xBF, '/', '?'}, {0xC0, '`', '~'}, {0xDB, '[', '{'}, {0xDC, '\\', '|'}, {0xDD, ']', '}'},
    {0xDE, '\'', '"'},
};

} // namespace

std::optional<KeyCode> keyFromUsage(platform::Scancode usage) {
    for (const Row &row : kKeys) {
        if (row.usage == usage) {
            return KeyCode{row.vk, row.sideVk, row.scan, row.extended};
        }
    }
    return std::nullopt;
}

int usagesOfVk(std::uint32_t vk, platform::Scancode out[2]) {
    int count = 0;
    for (const Row &row : kKeys) {
        if ((row.vk == vk || row.sideVk == vk) && count < 2) {
            if (row.usage == 0x58 && vk == kVkReturn && count == 1) {
                continue; // keypad Enter is not a second Return here
            }
            out[count++] = row.usage;
        }
    }
    return count;
}

std::uint32_t keyLParam(const KeyCode &key, bool down, bool repeat) {
    std::uint32_t value = 1u | static_cast<std::uint32_t>(key.scan) << 16;
    if (key.extended) {
        value |= 1u << 24;
    }
    if (!down) {
        value |= 3u << 30; // previous state down, transition up
    } else if (repeat) {
        value |= 1u << 30; // previous state down
    }
    return value;
}

char charFromVk(std::uint32_t vk, bool shift, bool control, bool capsLock) {
    if (vk >= 'A' && vk <= 'Z') {
        if (control) {
            return static_cast<char>(vk - 'A' + 1);
        }
        const bool upper = shift != capsLock;
        return static_cast<char>(upper ? vk : vk - 'A' + 'a');
    }
    if (control) {
        return 0;
    }
    if (vk >= '0' && vk <= '9') {
        return shift ? kShiftedDigits[vk - '0'] : static_cast<char>(vk);
    }
    if (vk >= 0x60 && vk <= 0x69) {
        return static_cast<char>('0' + (vk - 0x60));
    }
    switch (vk) {
    case kVkSpace:
        return ' ';
    case kVkReturn:
        return '\r';
    case kVkBack:
        return '\b';
    case kVkTab:
        return '\t';
    case kVkEscape:
        return 0x1B;
    case 0x6A:
        return '*';
    case 0x6B:
        return '+';
    case 0x6D:
        return '-';
    case 0x6E:
        return '.';
    case 0x6F:
        return '/';
    default:
        break;
    }
    for (const Oem &oem : kOem) {
        if (oem.vk == vk) {
            return shift ? oem.shifted : oem.plain;
        }
    }
    return 0;
}

std::uint32_t mapVirtualKey(std::uint32_t code, std::uint32_t mapType) {
    switch (mapType) {
    case 0:
        for (const Row &row : kKeys) {
            if ((row.vk == code || row.sideVk == code) && row.usage != 0x58) {
                return row.scan;
            }
        }
        return 0;
    case 1:
    case 3:
        for (const Row &row : kKeys) {
            if (row.scan == code && !row.extended) {
                return mapType == 1 ? row.vk : row.sideVk;
            }
        }
        return 0;
    case 2: {
        const char c = charFromVk(code, false, false, false);
        return code >= 'A' && code <= 'Z' ? code : static_cast<std::uint8_t>(c);
    }
    default:
        return 0;
    }
}

std::uint32_t vkKeyScan(char c) {
    if (c >= 'a' && c <= 'z') {
        return static_cast<std::uint32_t>(c - 'a' + 'A');
    }
    if (c >= 'A' && c <= 'Z') {
        return 0x100u | static_cast<std::uint32_t>(c);
    }
    if (c >= '0' && c <= '9') {
        return static_cast<std::uint32_t>(c);
    }
    switch (c) {
    case ' ':
        return kVkSpace;
    case '\r':
        return kVkReturn;
    case '\b':
        return kVkBack;
    case '\t':
        return kVkTab;
    case 0x1B:
        return kVkEscape;
    default:
        break;
    }
    if (c >= 1 && c <= 26) {
        return 0x200u | static_cast<std::uint32_t>('A' + c - 1);
    }
    if (const char *digit = std::strchr(kShiftedDigits, c); digit != nullptr && c != 0) {
        return 0x100u | static_cast<std::uint32_t>('0' + (digit - kShiftedDigits));
    }
    for (const Oem &oem : kOem) {
        if (oem.plain == c) {
            return oem.vk;
        }
        if (oem.shifted == c) {
            return 0x100u | oem.vk;
        }
    }
    return 0xFFFFu;
}

} // namespace sfcop::hle::user32
