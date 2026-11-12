// The extensions' opcodes (Extensions.h).
#include "app/scripts/interp/Extensions.h"

namespace sfcop::scripts::interp {

Extension extensionOf(std::uint8_t op2, bool prefixed) {
    switch (op2) {
    case 0x0D: case 0x0E: case 0x0F: // 3DNow!'s prefetch, femms, and its operations
        return prefixed ? Extension::kNone : Extension::k3dNow;
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x18:
    case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
    case 0x38: case 0x3A:
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
    case 0x7C: case 0x7D:
    case 0xAE: case 0xC2: case 0xC3: case 0xC6: case 0xD0:
        return Extension::kSse;
    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
    case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F:
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x7E: case 0x7F: case 0xC4: case 0xC5:
        return prefixed ? Extension::kSse : Extension::kMmx;
    default:
        if (op2 >= 0xD1 && op2 <= 0xFE) {
            return prefixed ? Extension::kSse : Extension::kMmx;
        }
        return Extension::kNone;
    }
}

bool extensionHasThirdByte(std::uint8_t op2) { return op2 == 0x38 || op2 == 0x3A; }

bool extensionHasModrm(std::uint8_t op2) { return op2 != 0x77 && op2 != 0x0E; }

bool extensionHasImmediate(std::uint8_t op2) {
    return op2 == 0x0F || op2 == 0x3A || (op2 >= 0x70 && op2 <= 0x73) || op2 == 0xC2 || op2 == 0xC4 ||
           op2 == 0xC5 || op2 == 0xC6;
}

const char *extensionArticleName(Extension extension) {
    switch (extension) {
    case Extension::kMmx: return "an MMX";
    case Extension::k3dNow: return "a 3DNow!";
    case Extension::kSse: return "an SSE";
    default: return "an";
    }
}

} // namespace sfcop::scripts::interp
