// The interpreter's working state for one entry into script code, and the flag rules.
//
// The registers live here while script code runs and go back to the Machine around every call
// out of it (as lifted code writes its local registers back around a call). The flags are the
// lifter's lazy triple: `kind` says which instruction wrote them, `a` and `b` are its operands (a
// narrow operand shifted up so its top bit is bit 31, the kind carrying that shift), and `carry`
// the carry that instructions keep for themselves (logic ops, shifts, inc and dec leave it to
// them). Every condition is derived from the triple when a jump asks for it.
#pragma once

#include "app/scripts/interp/Machine.h"

#include <cstdint>
#include <cstring>

namespace sfcop::scripts::interp {

class Interpreter;

// The lifter's flag kinds (recomp_types.h), in its numbering: the triple crosses calls as is.
enum FlagKind : std::uint32_t {
    kFlagsNone = 0,
    kFlagsCmp = 1,  // cmp, sub, neg: a - b
    kFlagsAdd = 2,  // add: a + b
    kFlagsTest = 3, // and, test (a & b); or, xor (the result in both)
    kFlagsBt = 4,   // bt: CF is bit b of a
    kFlagsFcom = 5, // a is -1, 0, 1, or 2 for unordered
    kFlagsInc = 6,  // a + b, CF kept
    kFlagsDec = 7,  // a - b, CF kept
    kFlagsWord = 8, // a is an EFLAGS word
};

inline std::uint32_t narrowKind(std::uint32_t kind, unsigned shift) { return kind | (shift << 8); }

enum Reg : unsigned { kEax, kEcx, kEdx, kEbx, kEsp, kEbp, kEsi, kEdi, kNoReg };

constexpr std::uint32_t kEflagsFixed = 0x00000202u;

struct Cpu {
    std::uint32_t r[9] = {}; // r[kNoReg] stays 0: an absent base or index adds nothing
    std::uint32_t kind = kFlagsNone;
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::uint32_t carry = 0;
    int direction = 1; // DF: +1 up, -1 down
    int fpuCompare = 0; // the last x87 compare: -1, 0, 1, 2 unordered; >= 0x10000 a status word
    std::uint32_t segment[3] = {}; // the bases of no override, fs:, gs:
    std::uint8_t *mem = nullptr;
    const Machine *machine = nullptr;
    Interpreter *interpreter = nullptr;
    std::uint32_t entryEsp = 0;  // esp at entry: the slot holding the caller's return address
    std::uint32_t function = 0;  // the guest function running, for g_cur_func
    std::uint32_t next = 0;      // where control goes when the current block ends
    bool leave = false;          // return to the host when the current block ends
    std::uint32_t entryRegs[8] = {}; // the registers at entry, for an entry that gives up
    int entryTop = 0;                // and the x87 depth

    // An access that would run past the guest's last byte, at strayAddress for straySize bytes
    // (0: none since the block began). It reads 0 and writes nothing; the run loop stops the script
    // when the block ends (Interpreter::run). Every access goes through fits() or load and store, so
    // no address the script makes reaches host memory outside the guest's 4 GiB.
    mutable std::uint32_t strayAddress = 0;
    mutable std::uint32_t straySize = 0;

    // Whether `size` bytes from `va` lie inside the guest's 4 GiB (they do not wrap).
    static bool fits(std::uint32_t va, std::uint64_t size) {
        return std::uint64_t{va} + size <= std::uint64_t{0x100000000ull};
    }
    void strayed(std::uint32_t va, std::uint64_t size) const {
        if (straySize == 0) {
            strayAddress = va;
            straySize = size > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<std::uint32_t>(size);
        }
    }
    // The host address of guest `va`, for an access of `size` bytes; null (and stray) past the end.
    std::uint8_t *at(std::uint32_t va, std::uint64_t size) const {
        if (!fits(va, size)) {
            strayed(va, size);
            return nullptr;
        }
        return mem + va;
    }
    template <class T> T load(std::uint32_t va) const {
        T v{};
        if (sizeof v == 1 || fits(va, sizeof v)) {
            std::memcpy(&v, mem + va, sizeof v);
        } else {
            strayed(va, sizeof v);
        }
        return v;
    }
    template <class T> void store(std::uint32_t va, T v) const {
        if (sizeof v == 1 || fits(va, sizeof v)) {
            std::memcpy(mem + va, &v, sizeof v);
        } else {
            strayed(va, sizeof v);
        }
    }
    std::uint32_t load32(std::uint32_t va) const { return load<std::uint32_t>(va); }
    void store32(std::uint32_t va, std::uint32_t v) const { store<std::uint32_t>(va, v); }
    void push(std::uint32_t v) {
        r[kEsp] -= 4;
        store32(r[kEsp], v);
    }
    std::uint32_t pop() {
        const std::uint32_t v = load32(r[kEsp]);
        r[kEsp] += 4;
        return v;
    }
};

inline std::uint32_t parity8(std::uint32_t v) {
    v &= 0xFFu;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return (~v) & 1u;
}

inline std::uint32_t packFlags(std::uint32_t r, std::uint32_t cf, std::uint32_t af, std::uint32_t of) {
    std::uint32_t e = kEflagsFixed | (cf & 1u) | ((af & 1u) << 4) | ((of & 1u) << 11);
    if (r == 0) {
        e |= 0x40u;
    }
    e |= (r >> 31) << 7;
    e |= parity8(r) << 2;
    return e;
}

// The flags as one EFLAGS word (recomp_eflags): PUSHFD, LAHF and every condition the fast paths
// below do not answer.
std::uint32_t eflagsWord(std::uint32_t kind, std::uint32_t a, std::uint32_t b, std::uint32_t carry, int direction);

inline std::uint32_t eflagsOf(const Cpu &c) { return eflagsWord(c.kind, c.a, c.b, c.carry, c.direction); }

// The carry as adc, sbb and the rotates through carry read it (recomp_carry): derived from an
// add's or a compare's operands, otherwise the one the last instruction kept.
inline std::uint32_t carryOf(const Cpu &c) {
    switch (c.kind & 0xFFu) {
    case kFlagsAdd:
        return c.a + c.b < c.a ? 1u : 0u;
    case kFlagsCmp:
        return c.a < c.b ? 1u : 0u;
    default:
        return c.carry & 1u;
    }
}

// The x86 condition `cc` (the low nibble of a jcc: o no b ae e ne be a s ns p np l ge le g).
bool conditionSlow(const Cpu &c, unsigned cc);

inline bool condition(const Cpu &c, unsigned cc) {
    const std::uint32_t a = c.a, b = c.b;
    switch (c.kind) { // wide kinds only; a narrow one carries its shift and takes the slow path
    case kFlagsCmp:
        switch (cc) {
        case 0x2: return a < b;
        case 0x3: return a >= b;
        case 0x4: return a == b;
        case 0x5: return a != b;
        case 0x6: return a <= b;
        case 0x7: return a > b;
        case 0x8: return ((a - b) >> 31) != 0;
        case 0x9: return ((a - b) >> 31) == 0;
        case 0xC: return static_cast<std::int32_t>(a) < static_cast<std::int32_t>(b);
        case 0xD: return static_cast<std::int32_t>(a) >= static_cast<std::int32_t>(b);
        case 0xE: return static_cast<std::int32_t>(a) <= static_cast<std::int32_t>(b);
        case 0xF: return static_cast<std::int32_t>(a) > static_cast<std::int32_t>(b);
        default: break;
        }
        break;
    case kFlagsTest: {
        const std::uint32_t r = a & b;
        switch (cc) {
        case 0x2: return false; // and, or, xor, test clear CF
        case 0x3: return true;
        case 0x4: case 0x6: return r == 0;
        case 0x5: case 0x7: return r != 0;
        case 0x8: case 0xC: return (r >> 31) != 0;
        case 0x9: case 0xD: return (r >> 31) == 0;
        case 0xE: return r == 0 || (r >> 31) != 0;
        case 0xF: return r != 0 && (r >> 31) == 0;
        default: break;
        }
        break;
    }
    default:
        break;
    }
    return conditionSlow(c, cc);
}

} // namespace sfcop::scripts::interp
