// The x87 instructions on the lifted game's register stack (Float.h).
#include "app/scripts/interp/Float.h"

#include <cmath>
#include <limits>
#include <utility>

namespace sfcop::scripts::interp {

namespace {

// Operand kinds of a memory form.
enum Kind : unsigned { kF32, kF64, kI16, kI32 };

inline double &st(const Cpu &c, unsigned n) { return c.machine->st[n]; }

inline void push(const Cpu &c, double v) {
    double *s = c.machine->st;
    std::int64_t *x = c.machine->stExact;
    for (unsigned n = 7; n > 0; --n) {
        s[n] = s[n - 1];
        x[n] = x[n - 1];
    }
    s[0] = v;
    x[0] = 0;
    ++*c.machine->fpTop;
}

inline double pop(const Cpu &c) {
    double *s = c.machine->st;
    std::int64_t *x = c.machine->stExact;
    const double v = s[0];
    for (unsigned n = 0; n < 7; ++n) {
        s[n] = s[n + 1];
        x[n] = x[n + 1];
    }
    s[7] = 0.0;
    x[7] = 0;
    --*c.machine->fpTop;
    return v;
}

inline double roundByControl(double v, std::uint16_t cw) {
    switch ((cw >> 10) & 3) {
    case 0: return std::nearbyint(v);
    case 1: return std::floor(v);
    case 2: return std::ceil(v);
    default: return std::trunc(v);
    }
}

inline int compare(double a, double b) { return a < b ? -1 : a > b ? 1 : a == b ? 0 : 2; }

template <unsigned K> inline double loadOperand(const Cpu &c, std::uint32_t va) {
    if constexpr (K == kF32) {
        return static_cast<double>(c.load<float>(va));
    } else if constexpr (K == kF64) {
        return c.load<double>(va);
    } else if constexpr (K == kI16) {
        return static_cast<double>(c.load<std::int16_t>(va));
    } else {
        return static_cast<double>(c.load<std::int32_t>(va));
    }
}

double load80(const std::uint8_t *p) {
    std::uint64_t m;
    std::uint16_t se;
    std::memcpy(&m, p, 8);
    std::memcpy(&se, p + 8, 2);
    const int e = se & 0x7FFF;
    double v;
    if (e == 0x7FFF) {
        v = (m << 1) != 0 ? NAN : INFINITY;
    } else {
        v = std::ldexp(static_cast<double>(m), (e != 0 ? e : 1) - 16383 - 63);
    }
    return (se & 0x8000) != 0 ? -v : v;
}

void store80(std::uint8_t *p, double v) {
    std::uint64_t m = 0;
    std::uint16_t se = std::signbit(v) ? 0x8000 : 0;
    if (std::isnan(v)) {
        se |= 0x7FFF;
        m = 0xC000000000000000ull;
    } else if (std::isinf(v)) {
        se |= 0x7FFF;
        m = 0x8000000000000000ull;
    } else if (v != 0.0) {
        int e;
        const double f = std::frexp(std::fabs(v), &e);
        m = static_cast<std::uint64_t>(std::ldexp(f, 64));
        se |= static_cast<std::uint16_t>(e - 1 + 16383);
    }
    std::memcpy(p, &m, 8);
    std::memcpy(p + 8, &se, 2);
}

// The arithmetic /digits: 0 add 1 mul 4 sub 5 subr 6 div 7 divr, as `dst op src`.
inline double arith(unsigned op, double dst, double src) {
    switch (op) {
    case 0: return dst + src;
    case 1: return dst * src;
    case 4: return dst - src;
    case 5: return src - dst;
    case 6: return dst / src;
    default: return src / dst;
    }
}

// D8 / DC / DA / DE with memory: st(0) op= [m], or a compare (/2 /3, /3 popping).
template <unsigned K> void arithMem(Cpu &c, const Insn &i) {
    const double v = loadOperand<K>(c, effectiveAddress(c, i));
    if (i.op == 2 || i.op == 3) {
        c.fpuCompare = compare(st(c, 0), v);
        if (i.op == 3) {
            pop(c);
        }
        return;
    }
    st(c, 0) = arith(i.op, st(c, 0), v);
}

// D8 with a register: st(0) op= st(i), or fcom / fcomp st(i).
void arithSt0(Cpu &c, const Insn &i) {
    if (i.op == 2 || i.op == 3) {
        c.fpuCompare = compare(st(c, 0), st(c, i.rm));
        if (i.op == 3) {
            pop(c);
        }
        return;
    }
    st(c, 0) = arith(i.op, st(c, 0), st(c, i.rm));
}

// DC with a register: st(i) op= st(0); here /4 is fsubr and /5 fsub, /6 fdivr and /7 fdiv.
inline unsigned swapReverse(unsigned op) { return op >= 4 ? op ^ 1u : op; }

void arithSti(Cpu &c, const Insn &i) { st(c, i.rm) = arith(swapReverse(i.op), st(c, i.rm), st(c, 0)); }

// DE with a register: as DC, then pop. A division by zero leaves the destination as it was (or,
// reversed, takes st(0)), as the lifted code does.
void arithStiPop(Cpu &c, const Insn &i) {
    const double top = st(c, 0);
    double &dst = st(c, i.rm);
    const unsigned op = swapReverse(i.op);
    if (op == 6) {
        dst = top == 0.0 ? dst : dst / top;
    } else if (op == 7) {
        dst = dst == 0.0 ? top : top / dst;
    } else {
        dst = arith(op, dst, top);
    }
    pop(c);
}

void fcompp(Cpu &c, const Insn &) {
    c.fpuCompare = compare(st(c, 0), st(c, 1));
    pop(c);
    pop(c);
}

// fcomi / fucomi (and the popping forms): straight into ZF PF CF.
template <bool Pop> void fcomi(Cpu &c, const Insn &i) {
    const double a = st(c, 0), b = st(c, i.rm);
    c.kind = kFlagsWord;
    c.a = a < b ? 0x01u : a > b ? 0x00u : a == b ? 0x40u : 0x45u;
    c.b = 0;
    c.carry = c.a & 1u;
    if (Pop) {
        pop(c);
    }
}

template <unsigned K> void fld(Cpu &c, const Insn &i) { push(c, loadOperand<K>(c, effectiveAddress(c, i))); }
void fld80(Cpu &c, const Insn &i) {
    const std::uint8_t *p = c.at(effectiveAddress(c, i), 10);
    push(c, p != nullptr ? load80(p) : 0.0);
}
void fild64(Cpu &c, const Insn &i) {
    const auto v = c.load<std::int64_t>(effectiveAddress(c, i));
    push(c, static_cast<double>(v));
    c.machine->stExact[0] = v;
}
void fldSt(Cpu &c, const Insn &i) { push(c, st(c, i.rm)); }
void fldConstant(Cpu &c, const Insn &i) {
    static constexpr double kConstants[] = {1.0, 3.321928094887362, 1.4426950408889634, 3.14159265358979323846,
                                            0.30102999566398120, 0.6931471805599453, 0.0};
    push(c, kConstants[i.rm]);
}

// fst / fstp to memory.
template <unsigned K, bool Pop> void fst(Cpu &c, const Insn &i) {
    const std::uint32_t va = effectiveAddress(c, i);
    const double v = Pop ? pop(c) : st(c, 0);
    if constexpr (K == kF32) {
        c.store<float>(va, static_cast<float>(v));
    } else {
        c.store<double>(va, v);
    }
}
void fstp80(Cpu &c, const Insn &i) {
    std::uint8_t *p = c.at(effectiveAddress(c, i), 10);
    const double v = pop(c);
    if (p != nullptr) {
        store80(p, v);
    }
}
// fst / fstp st(i): st(i) takes st(0), then (fstp) the stack pops.
template <bool Pop> void fstSt(Cpu &c, const Insn &i) {
    st(c, i.rm) = st(c, 0);
    c.machine->stExact[i.rm] = c.machine->stExact[0];
    if (Pop) {
        pop(c);
    }
}

// A rounded value as an integer of the instruction's width, as the x87 stores it: NaN, or a value
// outside the width's range, is the "integer indefinite", the width's most negative value. A C cast
// would be undefined there and differ by host (x86 gives the indefinite, arm64 saturates).
template <typename T> T toInteger(double r) {
    constexpr double low = static_cast<double>(std::numeric_limits<T>::min());
    constexpr double high = -low; // 2^(bits-1), the first value past the maximum
    if (!(r >= low && r < high)) {
        return std::numeric_limits<T>::min();
    }
    return static_cast<T>(r);
}

// fist / fistp, rounded by the control word.
template <unsigned K, bool Pop> void fist(Cpu &c, const Insn &i) {
    const std::uint32_t va = effectiveAddress(c, i);
    const double v = Pop ? pop(c) : st(c, 0);
    const double r = roundByControl(v, *c.machine->fpuControl);
    if constexpr (K == kI16) {
        c.store<std::int16_t>(va, toInteger<std::int16_t>(r));
    } else {
        c.store<std::uint32_t>(va, static_cast<std::uint32_t>(toInteger<std::int32_t>(r)));
    }
}
void fistp64(Cpu &c, const Insn &i) {
    const double v = st(c, 0);
    const std::int64_t shadow = c.machine->stExact[0];
    const std::int64_t q = v == static_cast<double>(shadow)
                               ? shadow
                               : toInteger<std::int64_t>(roundByControl(v, *c.machine->fpuControl));
    pop(c);
    c.store<std::int64_t>(effectiveAddress(c, i), q);
}

void fxch(Cpu &c, const Insn &i) {
    std::swap(st(c, 0), st(c, i.rm));
    std::swap(c.machine->stExact[0], c.machine->stExact[i.rm]);
}

void fldcw(Cpu &c, const Insn &i) { *c.machine->fpuControl = c.load<std::uint16_t>(effectiveAddress(c, i)); }
void fnstcw(Cpu &c, const Insn &i) { c.store<std::uint16_t>(effectiveAddress(c, i), *c.machine->fpuControl); }

std::uint32_t statusWord(const Cpu &c) {
    const int v = c.fpuCompare;
    if (v >= 0x10000) {
        return static_cast<std::uint32_t>(v) & 0x4700u;
    }
    return v == 2 ? 0x4500u : v < 0 ? 0x0100u : v == 0 ? 0x4000u : 0x0000u;
}
void fnstswAx(Cpu &c, const Insn &) { c.r[kEax] = (c.r[kEax] & 0xFFFF0000u) | statusWord(c); }
void fnstswMem(Cpu &c, const Insn &i) {
    c.store<std::uint16_t>(effectiveAddress(c, i), static_cast<std::uint16_t>(statusWord(c)));
}

void fnop(Cpu &, const Insn &) {}

// D9 E0 to FF: one handler, the low six bits of the ModRM in i.rm's place (i.imm).
void d9(Cpu &c, const Insn &i) {
    double &s0 = st(c, 0);
    switch (i.imm) {
    case 0xE0: s0 = -s0; break;
    case 0xE1: s0 = std::fabs(s0); break;
    case 0xE4: c.fpuCompare = compare(s0, 0.0); break;
    case 0xE5: {
        const double v = s0;
        std::uint32_t cc = std::isnan(v) ? 0x0100u : std::isinf(v) ? 0x0500u : v == 0.0 ? 0x4000u
                           : std::fabs(v) < 2.2250738585072014e-308 ? 0x4400u : 0x0400u;
        if (std::signbit(v)) {
            cc |= 0x0200u;
        }
        c.fpuCompare = static_cast<int>(0x10000u | cc);
        break;
    }
    case 0xF0: s0 = std::pow(2.0, s0) - 1.0; break;
    case 0xF1: {
        const double x = pop(c);
        st(c, 0) *= std::log(x) / 0.6931471805599453;
        break;
    }
    case 0xF2:
        s0 = std::tan(s0);
        push(c, 1.0);
        c.fpuCompare = 1;
        break;
    case 0xF3: {
        const double x = pop(c);
        st(c, 0) = std::atan2(st(c, 0), x);
        break;
    }
    case 0xF5: case 0xF8:
        s0 = std::fmod(s0, st(c, 1));
        c.fpuCompare = 1;
        break;
    case 0xF6: push(c, 0.0); break;
    case 0xF7: pop(c); break;
    case 0xF9: {
        const double x = pop(c);
        st(c, 0) *= std::log(x + 1.0) / 0.6931471805599453;
        break;
    }
    case 0xFA: s0 = std::sqrt(s0); break;
    case 0xFB: {
        const double a = s0;
        s0 = std::cos(a);
        push(c, std::sin(a));
        c.fpuCompare = 1;
        break;
    }
    case 0xFC: s0 = roundByControl(s0, *c.machine->fpuControl); break;
    case 0xFD: s0 *= std::pow(2.0, static_cast<double>(static_cast<int>(st(c, 1)))); break;
    case 0xFE: s0 = std::sin(s0); c.fpuCompare = 1; break;
    case 0xFF: s0 = std::cos(s0); c.fpuCompare = 1; break;
    default: break;
    }
}

bool knownD9(unsigned byte) {
    switch (byte) {
    case 0xE0: case 0xE1: case 0xE4: case 0xE5: case 0xF0: case 0xF1: case 0xF2: case 0xF3:
    case 0xF5: case 0xF6: case 0xF7: case 0xF8: case 0xF9: case 0xFA: case 0xFB: case 0xFC:
    case 0xFD: case 0xFE: case 0xFF:
        return true;
    default:
        return false;
    }
}

} // namespace

bool decodeFloat(std::uint8_t op, Insn &i) {
    const bool m = i.rm == kMem;
    const unsigned d = i.op;
    if (m) {
        switch (op) {
        case 0xD8: i.run = &arithMem<kF32>; return true;
        case 0xDC: i.run = &arithMem<kF64>; return true;
        case 0xDA: i.run = &arithMem<kI32>; return true;
        case 0xDE: i.run = &arithMem<kI16>; return true;
        case 0xD9:
            switch (d) {
            case 0: i.run = &fld<kF32>; return true;
            case 2: i.run = &fst<kF32, false>; return true;
            case 3: i.run = &fst<kF32, true>; return true;
            case 5: i.run = &fldcw; return true;
            case 7: i.run = &fnstcw; return true;
            default: return false;
            }
        case 0xDD:
            switch (d) {
            case 0: i.run = &fld<kF64>; return true;
            case 2: i.run = &fst<kF64, false>; return true;
            case 3: i.run = &fst<kF64, true>; return true;
            case 4: case 6: i.run = &fnop; return true; // frstor, fnsave: the stack is not a memory image, as the lifted code has it
            case 7: i.run = &fnstswMem; return true;
            default: return false;
            }
        case 0xDB:
            switch (d) {
            case 0: i.run = &fld<kI32>; return true;
            case 2: i.run = &fist<kI32, false>; return true;
            case 3: i.run = &fist<kI32, true>; return true;
            case 5: i.run = &fld80; return true;
            case 7: i.run = &fstp80; return true;
            default: return false;
            }
        default: // 0xDF
            switch (d) {
            case 0: i.run = &fld<kI16>; return true;
            case 2: i.run = &fist<kI16, false>; return true;
            case 3: i.run = &fist<kI16, true>; return true;
            case 5: i.run = &fild64; return true;
            case 7: i.run = &fistp64; return true;
            default: return false;
            }
        }
    }
    const unsigned modrm = 0xC0u | (d << 3) | i.rm;
    switch (op) {
    case 0xD8: i.run = &arithSt0; return true;
    case 0xDC:
        i.run = d == 2 || d == 3 ? &arithSt0 : &arithSti;
        return true;
    case 0xDE:
        if (d == 3) {
            i.run = modrm == 0xD9 ? &fcompp : nullptr;
            return modrm == 0xD9;
        }
        if (d == 2) {
            return false;
        }
        i.run = &arithStiPop;
        return true;
    case 0xD9:
        if (d == 0) {
            i.run = &fldSt;
            return true;
        }
        if (d == 1) {
            i.run = &fxch;
            return true;
        }
        if (modrm == 0xD0) {
            i.run = &fnop;
            return true;
        }
        if (modrm >= 0xE8 && modrm <= 0xEE) {
            i.rm = static_cast<std::uint8_t>(modrm - 0xE8);
            i.run = &fldConstant;
            return true;
        }
        i.imm = modrm;
        i.run = &d9;
        return knownD9(modrm);
    case 0xDA:
        if (modrm == 0xE9) {
            i.run = &fcompp; // fucompp
            return true;
        }
        return false;
    case 0xDB:
        if (modrm == 0xE2 || modrm == 0xE3) {
            i.run = &fnop; // fnclex, fninit: no exception state is modelled
            return true;
        }
        if (d == 5 || d == 6) {
            i.run = &fcomi<false>;
            return true;
        }
        return false;
    case 0xDD:
        switch (d) {
        case 0: i.run = &fnop; return true; // ffree, as the lifted code has it
        case 2: i.run = &fstSt<false>; return true;
        case 3: i.run = &fstSt<true>; return true;
        case 4: case 5: {
            i.op = d == 4 ? 2 : 3; // fucom, fucomp: as fcom, fcomp
            i.run = &arithSt0;
            return true;
        }
        default: return false;
        }
    default: // 0xDF
        if (modrm == 0xE0) {
            i.run = &fnstswAx;
            return true;
        }
        if (d == 5 || d == 6) {
            i.run = &fcomi<true>;
            return true;
        }
        if (d == 0) {
            i.run = &fnop; // ffreep, as the lifted code has it
            return true;
        }
        return false;
    }
}

} // namespace sfcop::scripts::interp
