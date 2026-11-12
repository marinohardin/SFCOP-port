// The flag words and the conditions the fast paths in Cpu.h leave to here, derived from the lazy
// triple by the lifter's rules, so a branch reads the same answer whichever side wrote the flags.
#include "app/scripts/interp/Cpu.h"

namespace sfcop::scripts::interp {

std::uint32_t eflagsWord(std::uint32_t kind, std::uint32_t a, std::uint32_t b, std::uint32_t carry,
                         int direction) {
    const unsigned shift = kind >> 8;
    kind &= 0xFFu;
    std::uint32_t cf = carry & 1u, of = 0, af = 0, r = 0, e = 0;
    const std::uint32_t df = direction < 0 ? 0x400u : 0u;
    switch (kind) {
    case kFlagsWord:
        // Every arithmetic flag from its own bit; AC and ID ride along for CPU probes.
        return (a & (1u | 4u | 0x10u | 0x40u | 0x80u | 0x800u | 0x40000u | 0x200000u)) |
               kEflagsFixed | df;
    case kFlagsAdd:
    case kFlagsInc:
        r = a + b;
        if (kind == kFlagsAdd) {
            cf = r < a ? 1u : 0u;
        }
        of = (~(a ^ b) & (a ^ r)) >> 31;
        af = ((a ^ b ^ r) >> (shift + 4)) & 1u;
        break;
    case kFlagsTest:
        r = a & b;
        break;
    case kFlagsBt:
        r = 0;
        cf = (a >> (b & 31)) & 1u;
        break;
    case kFlagsNone:
    case kFlagsFcom:
        return kEflagsFixed | cf | df;
    default: // compare, dec
        r = a - b;
        if (kind == kFlagsCmp) {
            cf = a < b ? 1u : 0u;
        }
        of = ((a ^ b) & (a ^ r)) >> 31;
        af = ((a ^ b ^ r) >> (shift + 4)) & 1u;
        break;
    }
    e = packFlags(r, cf, af, of);
    if (shift != 0) {
        e = (e & ~4u) | (parity8(r >> shift) << 2);
    }
    return e | df;
}

bool conditionSlow(const Cpu &c, unsigned cc) {
    const std::uint32_t kind = c.kind & 0xFFu;
    const unsigned shift = c.kind >> 8;
    const std::uint32_t a = c.a, b = c.b;
    if (kind == kFlagsFcom) {
        const auto v = static_cast<std::int32_t>(a);
        if (v == 2) { // unordered: ZF, PF and CF set
            return cc == 0x2 || cc == 0x4 || cc == 0x6 || cc == 0xA;
        }
        switch (cc) {
        case 0xB: return true;
        case 0x4: return v == 0;
        case 0x5: return v != 0;
        case 0x2: case 0xC: return v < 0;
        case 0x6: case 0xE: return v <= 0;
        case 0x7: case 0xF: return v > 0;
        case 0x3: case 0xD: return v >= 0;
        default: return false;
        }
    }
    std::uint32_t zf = 0, sf = 0, cf = 0, of = 0, pf = 0;
    if (kind == kFlagsWord) {
        cf = a & 1u;
        pf = (a >> 2) & 1u;
        zf = (a >> 6) & 1u;
        sf = (a >> 7) & 1u;
        of = (a >> 11) & 1u;
    } else {
        std::uint32_t r = 0;
        switch (kind) {
        case kFlagsAdd:
        case kFlagsInc:
            r = a + b;
            cf = r < a ? 1u : 0u;
            of = (~(a ^ b) & (a ^ r)) >> 31;
            break;
        case kFlagsTest:
            r = a & b;
            break;
        case kFlagsBt:
            r = 0;
            cf = (a >> (b & 31)) & 1u;
            break;
        default: // compare, dec, none
            r = a - b;
            cf = a < b ? 1u : 0u;
            of = ((a ^ b) & (a ^ r)) >> 31;
            break;
        }
        if (kind == kFlagsInc || kind == kFlagsDec) {
            cf = c.carry & 1u; // inc and dec keep the carry from before them
        }
        zf = r == 0 ? 1u : 0u;
        sf = r >> 31;
        pf = parity8(r >> shift);
    }
    switch (cc) {
    case 0x0: return of != 0;
    case 0x1: return of == 0;
    case 0x2: return cf != 0;
    case 0x3: return cf == 0;
    case 0x4: return zf != 0;
    case 0x5: return zf == 0;
    case 0x6: return cf != 0 || zf != 0;
    case 0x7: return cf == 0 && zf == 0;
    case 0x8: return sf != 0;
    case 0x9: return sf == 0;
    case 0xA: return pf != 0;
    case 0xB: return pf == 0;
    case 0xC: return sf != of;
    case 0xD: return sf == of;
    case 0xE: return zf != 0 || sf != of;
    default: return zf == 0 && sf == of;
    }
}

} // namespace sfcop::scripts::interp
