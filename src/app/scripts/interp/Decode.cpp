// The decoder and the integer instructions: what MSVC 6 emits for a mission script and its
// statically linked C runtime, in 32-bit flat code (no 16-bit addressing, no far control).
//
// Every instruction does what the x86 does, and leaves its flags in the lifter's representation
// (Cpu.h): add, sub, cmp, neg, inc, dec, logic and test as the lazy triple; everything the triple
// cannot say (adc, sbb, the shifts and rotates, multiplies) as a finished word.
#include "app/scripts/interp/Extensions.h"
#include "app/scripts/interp/Float.h"
#include "app/scripts/interp/Interpreter.h"

#include <chrono>
#include <cstdio>
#include <limits>

namespace sfcop::scripts::interp {

namespace {

// --- The ALU: add or adc sbb and sub xor cmp, in that /digit order; 8 is test. ---

template <unsigned Op, unsigned S> inline std::uint32_t alu(Cpu &c, std::uint32_t a, std::uint32_t b) {
    constexpr std::uint32_t M = kMask<S>;
    constexpr unsigned top = kBits<S> - 1;
    std::uint32_t r = 0;
    if constexpr (Op == 0) {
        r = (a + b) & M;
        setFlags<S>(c, kFlagsAdd, a, b);
        c.carry = S == 4 ? (r < a ? 1u : 0u) : ((a + b) > M ? 1u : 0u);
    } else if constexpr (Op == 1 || Op == 6) {
        r = Op == 1 ? (a | b) : (a ^ b);
        setFlags<S>(c, kFlagsTest, r, r);
        c.carry = 0;
    } else if constexpr (Op == 2) {
        const std::uint64_t cin = carryOf(c);
        const std::uint64_t full = std::uint64_t{a} + b + cin;
        r = static_cast<std::uint32_t>(full) & M;
        setFlagWord(c, resultWord<S>(r, full > M ? 1u : 0u, ((a ^ b ^ r) >> 4) & 1u,
                                     ((~(a ^ b) & (a ^ r)) >> top) & 1u));
    } else if constexpr (Op == 3) {
        const std::uint64_t cin = carryOf(c);
        r = (a - b - static_cast<std::uint32_t>(cin)) & M;
        setFlagWord(c, resultWord<S>(r, std::uint64_t{b} + cin > a ? 1u : 0u, ((a ^ b ^ r) >> 4) & 1u,
                                     (((a ^ b) & (a ^ r)) >> top) & 1u));
    } else if constexpr (Op == 4 || Op == 8) {
        r = a & b;
        setFlags<S>(c, kFlagsTest, a, b);
        c.carry = 0;
    } else { // 5 sub, 7 cmp
        r = (a - b) & M;
        setFlags<S>(c, kFlagsCmp, a, b);
        c.carry = a < b ? 1u : 0u;
    }
    return r;
}

template <unsigned Op> constexpr bool kWrites = Op != 7 && Op != 8;

template <unsigned Op, unsigned S, bool M> void aluRmReg(Cpu &c, const Insn &i) {
    if constexpr (M) {
        const std::uint32_t va = effectiveAddress(c, i);
        const std::uint32_t r = alu<Op, S>(c, loadN<S>(c, va), getReg<S>(c, i.reg));
        if constexpr (kWrites<Op>) {
            storeN<S>(c, va, r);
        }
    } else {
        const std::uint32_t r = alu<Op, S>(c, getReg<S>(c, i.rm), getReg<S>(c, i.reg));
        if constexpr (kWrites<Op>) {
            setReg<S>(c, i.rm, r);
        }
    }
}

template <unsigned Op, unsigned S, bool M> void aluRegRm(Cpu &c, const Insn &i) {
    const std::uint32_t r = alu<Op, S>(c, getReg<S>(c, i.reg), readRm<S, M>(c, i));
    if constexpr (kWrites<Op>) {
        setReg<S>(c, i.reg, r);
    }
}

template <unsigned Op, unsigned S, bool M> void aluRmImm(Cpu &c, const Insn &i) {
    if constexpr (M) {
        const std::uint32_t va = effectiveAddress(c, i);
        const std::uint32_t r = alu<Op, S>(c, loadN<S>(c, va), i.imm & kMask<S>);
        if constexpr (kWrites<Op>) {
            storeN<S>(c, va, r);
        }
    } else {
        const std::uint32_t r = alu<Op, S>(c, getReg<S>(c, i.rm), i.imm & kMask<S>);
        if constexpr (kWrites<Op>) {
            setReg<S>(c, i.rm, r);
        }
    }
}

#define SIZED(F, s, m)                                                                             \
    ((s) == 1 ? ((m) ? F(1, true) : F(1, false))                                                   \
              : (s) == 2 ? ((m) ? F(2, true) : F(2, false)) : ((m) ? F(4, true) : F(4, false)))

enum class Form { RmReg, RegRm, RmImm };

template <unsigned Op> Handler aluFor(Form form, unsigned s, bool m) {
#define RMREG(S, M) &aluRmReg<Op, S, M>
#define REGRM(S, M) &aluRegRm<Op, S, M>
#define RMIMM(S, M) &aluRmImm<Op, S, M>
    switch (form) {
    case Form::RmReg: return SIZED(RMREG, s, m);
    case Form::RegRm: return SIZED(REGRM, s, m);
    default: return SIZED(RMIMM, s, m);
    }
#undef RMREG
#undef REGRM
#undef RMIMM
}

Handler aluHandler(unsigned op, Form form, unsigned s, bool m) {
    switch (op) {
    case 0: return aluFor<0>(form, s, m);
    case 1: return aluFor<1>(form, s, m);
    case 2: return aluFor<2>(form, s, m);
    case 3: return aluFor<3>(form, s, m);
    case 4: return aluFor<4>(form, s, m);
    case 5: return aluFor<5>(form, s, m);
    case 6: return aluFor<6>(form, s, m);
    case 7: return aluFor<7>(form, s, m);
    default: return aluFor<8>(form, s, m);
    }
}

// --- Moves. ---

template <unsigned S, bool M> void movRmReg(Cpu &c, const Insn &i) { writeRm<S, M>(c, i, getReg<S>(c, i.reg)); }
template <unsigned S, bool M> void movRegRm(Cpu &c, const Insn &i) { setReg<S>(c, i.reg, readRm<S, M>(c, i)); }
template <unsigned S, bool M> void movRmImm(Cpu &c, const Insn &i) { writeRm<S, M>(c, i, i.imm); }

// movzx / movsx: From-byte source, S-byte destination register.
template <unsigned From, bool Signed, unsigned S, bool M> void movExtend(Cpu &c, const Insn &i) {
    std::uint32_t v = readRm<From, M>(c, i);
    if constexpr (Signed) {
        v = From == 1 ? static_cast<std::uint32_t>(static_cast<std::int8_t>(v))
                      : static_cast<std::uint32_t>(static_cast<std::int16_t>(v));
    }
    setReg<S>(c, i.reg, v);
}

template <unsigned S> void lea(Cpu &c, const Insn &i) {
    setReg<S>(c, i.reg, c.r[i.base] + (c.r[i.index] << i.scale) + i.disp);
}

template <unsigned S, bool M> void xchg(Cpu &c, const Insn &i) {
    if constexpr (M) {
        const std::uint32_t va = effectiveAddress(c, i);
        const std::uint32_t v = loadN<S>(c, va);
        storeN<S>(c, va, getReg<S>(c, i.reg));
        setReg<S>(c, i.reg, v);
    } else {
        const std::uint32_t v = getReg<S>(c, i.rm);
        setReg<S>(c, i.rm, getReg<S>(c, i.reg));
        setReg<S>(c, i.reg, v);
    }
}

void nop(Cpu &, const Insn &) {}

// --- The stack. ---

void pushReg(Cpu &c, const Insn &i) { c.push(c.r[i.reg]); }
void popReg(Cpu &c, const Insn &i) { c.r[i.reg] = c.pop(); }
void pushImm(Cpu &c, const Insn &i) { c.push(i.imm); }
template <bool M> void pushRm(Cpu &c, const Insn &i) { c.push(readRm<4, M>(c, i)); }
template <bool M> void popRm(Cpu &c, const Insn &i) {
    const std::uint32_t v = c.pop(); // the address is formed after esp moves
    writeRm<4, M>(c, i, v);
}
void pushad(Cpu &c, const Insn &) {
    const std::uint32_t esp = c.r[kEsp];
    for (unsigned n = 0; n < 8; ++n) {
        c.push(n == kEsp ? esp : c.r[n]);
    }
}
void popad(Cpu &c, const Insn &) {
    for (unsigned n = 8; n-- > 0;) {
        const std::uint32_t v = c.pop();
        if (n != kEsp) {
            c.r[n] = v;
        }
    }
}
void pushfd(Cpu &c, const Insn &) { c.push(eflagsOf(c)); }
void popfd(Cpu &c, const Insn &) {
    const std::uint32_t v = c.pop();
    c.kind = kFlagsWord;
    c.a = v;
    c.b = 0;
    c.carry = v & 1u;
    c.direction = (v & 0x400u) != 0 ? -1 : 1;
}
void leave(Cpu &c, const Insn &) {
    c.r[kEsp] = c.r[kEbp];
    c.r[kEbp] = c.pop();
}
void enter(Cpu &c, const Insn &i) {
    c.push(c.r[kEbp]);
    c.r[kEbp] = c.r[kEsp];
    c.r[kEsp] -= i.imm;
}

// --- Unary groups. ---

template <unsigned S, bool M> void incRm(Cpu &c, const Insn &i) {
    const std::uint32_t a = readRm<S, M>(c, i);
    setFlags<S>(c, kFlagsInc, a, 1);
    writeRm<S, M>(c, i, (a + 1) & kMask<S>);
}
template <unsigned S, bool M> void decRm(Cpu &c, const Insn &i) {
    const std::uint32_t a = readRm<S, M>(c, i);
    setFlags<S>(c, kFlagsDec, a, 1);
    writeRm<S, M>(c, i, (a - 1) & kMask<S>);
}
template <unsigned S, bool M> void notRm(Cpu &c, const Insn &i) { writeRm<S, M>(c, i, ~readRm<S, M>(c, i)); }
template <unsigned S, bool M> void negRm(Cpu &c, const Insn &i) {
    const std::uint32_t a = readRm<S, M>(c, i);
    setFlags<S>(c, kFlagsCmp, 0, a);
    c.carry = a != 0 ? 1u : 0u;
    writeRm<S, M>(c, i, (0u - a) & kMask<S>);
}

inline void multiplyFlags(Cpu &c, bool overflow) {
    setFlagWord(c, kEflagsFixed | (overflow ? 0x801u : 0u));
}

template <unsigned S, bool M> void mulRm(Cpu &c, const Insn &i) {
    const std::uint64_t src = readRm<S, M>(c, i);
    if constexpr (S == 4) {
        const std::uint64_t p = std::uint64_t{c.r[kEax]} * src;
        c.r[kEax] = static_cast<std::uint32_t>(p);
        c.r[kEdx] = static_cast<std::uint32_t>(p >> 32);
        multiplyFlags(c, c.r[kEdx] != 0);
    } else if constexpr (S == 2) {
        const std::uint32_t p = (c.r[kEax] & 0xFFFFu) * static_cast<std::uint32_t>(src);
        setReg<2>(c, kEax, p);
        setReg<2>(c, kEdx, p >> 16);
        multiplyFlags(c, (p >> 16) != 0);
    } else {
        const std::uint32_t p = (c.r[kEax] & 0xFFu) * static_cast<std::uint32_t>(src);
        setReg<2>(c, kEax, p);
        multiplyFlags(c, (p >> 8) != 0);
    }
}

template <unsigned S, bool M> void imulRm(Cpu &c, const Insn &i) {
    if constexpr (S == 4) {
        const std::int64_t p = std::int64_t{static_cast<std::int32_t>(c.r[kEax])} *
                               static_cast<std::int32_t>(readRm<4, M>(c, i));
        c.r[kEax] = static_cast<std::uint32_t>(p);
        c.r[kEdx] = static_cast<std::uint32_t>(static_cast<std::uint64_t>(p) >> 32);
        multiplyFlags(c, p != static_cast<std::int32_t>(p));
    } else if constexpr (S == 2) {
        const std::int32_t p = std::int32_t{static_cast<std::int16_t>(c.r[kEax])} *
                               static_cast<std::int16_t>(readRm<2, M>(c, i));
        setReg<2>(c, kEax, static_cast<std::uint32_t>(p));
        setReg<2>(c, kEdx, static_cast<std::uint32_t>(p) >> 16);
        multiplyFlags(c, p != static_cast<std::int16_t>(p));
    } else {
        const std::int32_t p = std::int32_t{static_cast<std::int8_t>(c.r[kEax])} *
                               static_cast<std::int8_t>(readRm<1, M>(c, i));
        setReg<2>(c, kEax, static_cast<std::uint32_t>(p));
        multiplyFlags(c, p != static_cast<std::int8_t>(p));
    }
}

// imul r, r/m (, imm): the low half; the flags say whether it overflowed.
template <unsigned S, bool M, bool Imm> void imulReg(Cpu &c, const Insn &i) {
    const std::int64_t a = Imm ? static_cast<std::int32_t>(i.imm) : static_cast<std::int32_t>(getReg<4>(c, i.reg));
    const std::uint32_t raw = readRm<S, M>(c, i);
    const std::int64_t b = S == 4 ? static_cast<std::int32_t>(raw) : static_cast<std::int16_t>(raw);
    const std::int64_t aa = S == 4 ? a : static_cast<std::int16_t>(a);
    const std::int64_t p = aa * b;
    setReg<S>(c, i.reg, static_cast<std::uint32_t>(p));
    multiplyFlags(c, S == 4 ? p != static_cast<std::int32_t>(p) : p != static_cast<std::int16_t>(p));
}

template <unsigned S, bool M, bool Signed> void divRm(Cpu &c, const Insn &i) {
    const std::uint32_t d = readRm<S, M>(c, i);
    // The dividend, the divisor and the quotient's range, all in 64 bits so no case is undefined.
    std::int64_t n = 0;
    std::int64_t sd = 0;
    std::int64_t low = 0;
    std::int64_t high = 0;
    if constexpr (S == 4) {
        const std::uint64_t u = (std::uint64_t{c.r[kEdx]} << 32) | c.r[kEax];
        n = static_cast<std::int64_t>(u);
        sd = Signed ? std::int64_t{static_cast<std::int32_t>(d)} : std::int64_t{d};
        low = Signed ? -0x80000000LL : 0;
        high = Signed ? 0x7FFFFFFFLL : 0xFFFFFFFFLL;
    } else if constexpr (S == 2) {
        const std::uint32_t u = ((c.r[kEdx] & 0xFFFFu) << 16) | (c.r[kEax] & 0xFFFFu);
        n = Signed ? std::int64_t{static_cast<std::int32_t>(u)} : std::int64_t{u};
        sd = Signed ? std::int64_t{static_cast<std::int16_t>(d)} : std::int64_t{d};
        low = Signed ? -0x8000LL : 0;
        high = Signed ? 0x7FFFLL : 0xFFFFLL;
    } else {
        const std::uint32_t u = c.r[kEax] & 0xFFFFu;
        n = Signed ? std::int64_t{static_cast<std::int16_t>(u)} : std::int64_t{u};
        sd = Signed ? std::int64_t{static_cast<std::int8_t>(d)} : std::int64_t{d};
        low = Signed ? -0x80LL : 0;
        high = Signed ? 0x7FLL : 0xFFLL;
    }
    std::int64_t q = 0;
    std::int64_t r = 0;
    bool fits = sd != 0;
    if (fits) {
        if constexpr (S == 4 && !Signed) {
            const std::uint64_t un = static_cast<std::uint64_t>(n);
            const auto ud = static_cast<std::uint64_t>(sd);
            fits = un / ud <= 0xFFFFFFFFull;
            q = static_cast<std::int64_t>(un / ud);
            r = static_cast<std::int64_t>(un % ud);
        } else if (S == 4 && sd == -1) {
            // -n overflows 64 bits only for the smallest n, whose quotient does not fit anyway.
            fits = n != std::numeric_limits<std::int64_t>::min() && -n >= low && -n <= high;
            q = fits ? -n : 0;
        } else {
            q = n / sd;
            r = n % sd;
            fits = q >= low && q <= high;
        }
    }
    if (!fits) {
        // #DE, for a zero divisor or a quotient that does not fit. As lifted code: the quotient
        // and remainder read 0 when the fault handler returns.
        c.interpreter->divideFault(c, sd == 0 ? kDivideByZero : kIntegerOverflow);
        q = 0;
        r = 0;
    }
    const auto uq = static_cast<std::uint32_t>(q);
    const auto ur = static_cast<std::uint32_t>(r);
    if constexpr (S == 1) {
        setReg<2>(c, kEax, (uq & 0xFFu) | ((ur & 0xFFu) << 8));
    } else {
        setReg<S>(c, kEax, uq);
        setReg<S>(c, kEdx, ur);
    }
}

// --- Shifts and rotates: /digit 0 rol 1 ror 2 rcl 3 rcr 4 shl 5 shr 6 sal 7 sar. Count from the
// immediate (1 included) or from cl, masked to 5 bits; a zero count changes nothing. ---

template <unsigned Op, unsigned S> std::uint32_t shift(Cpu &c, std::uint32_t a, unsigned count) {
    constexpr unsigned bits = kBits<S>;
    constexpr std::uint32_t M = kMask<S>;
    std::uint32_t r = a, cf = 0, of = 0;
    if constexpr (Op == 4 || Op == 6) {
        const std::uint64_t wide = std::uint64_t{a} << count;
        r = static_cast<std::uint32_t>(wide) & M;
        cf = static_cast<std::uint32_t>(wide >> bits) & 1u;
        of = ((r >> (bits - 1)) & 1u) ^ cf;
    } else if constexpr (Op == 5) {
        cf = count <= bits ? (a >> (count - 1)) & 1u : 0u;
        r = count < bits ? a >> count : 0u;
        of = (a >> (bits - 1)) & 1u;
    } else if constexpr (Op == 7) {
        const std::int64_t sa = S == 4 ? std::int64_t{static_cast<std::int32_t>(a)}
                                : S == 2 ? std::int64_t{static_cast<std::int16_t>(a)}
                                         : std::int64_t{static_cast<std::int8_t>(a)};
        cf = static_cast<std::uint32_t>(sa >> (count - 1 < 63 ? count - 1 : 63)) & 1u;
        r = static_cast<std::uint32_t>(sa >> count) & M;
        of = 0;
    }
    if constexpr (Op >= 4) {
        setFlagWord(c, resultWord<S>(r, cf, 0, count == 1 ? of : 0));
        return r;
    } else {
        // Rotates change CF (and OF, for a count of 1) and keep every other flag.
        if constexpr (Op == 0) {
            const unsigned n = count % bits;
            r = n == 0 ? a : ((a << n) | (a >> (bits - n))) & M;
            cf = r & 1u;
            of = ((r >> (bits - 1)) & 1u) ^ cf;
        } else if constexpr (Op == 1) {
            const unsigned n = count % bits;
            r = n == 0 ? a : ((a >> n) | (a << (bits - n))) & M;
            cf = (r >> (bits - 1)) & 1u;
            of = ((r >> (bits - 1)) ^ (r >> (bits - 2))) & 1u;
        } else {
            cf = carryOf(c);
            const unsigned n = count % (bits + 1);
            for (unsigned k = 0; k < n; ++k) {
                if constexpr (Op == 2) {
                    const std::uint32_t out = (r >> (bits - 1)) & 1u;
                    r = ((r << 1) | cf) & M;
                    cf = out;
                } else {
                    const std::uint32_t out = r & 1u;
                    r = (r >> 1) | (cf << (bits - 1));
                    cf = out;
                }
            }
            of = Op == 2 ? ((r >> (bits - 1)) & 1u) ^ cf : ((r >> (bits - 1)) ^ (r >> (bits - 2))) & 1u;
        }
        std::uint32_t word = (eflagsOf(c) & ~0x1u) | cf;
        if (count == 1) {
            word = (word & ~0x800u) | (of << 11);
        }
        setFlagWord(c, word);
        return r;
    }
}

template <unsigned Op, unsigned S, bool M, bool ByCl> void shiftRm(Cpu &c, const Insn &i) {
    const unsigned count = (ByCl ? c.r[kEcx] : i.imm) & 31u;
    if (count == 0) {
        return;
    }
    if constexpr (M) {
        const std::uint32_t va = effectiveAddress(c, i);
        storeN<S>(c, va, shift<Op, S>(c, loadN<S>(c, va), count));
    } else {
        setReg<S>(c, i.rm, shift<Op, S>(c, getReg<S>(c, i.rm), count));
    }
}

template <unsigned Op, bool ByCl> Handler shiftFor(unsigned s, bool m) {
#define SH(S, M) &shiftRm<Op, S, M, ByCl>
    return SIZED(SH, s, m);
#undef SH
}

template <bool ByCl> Handler shiftHandler(unsigned op, unsigned s, bool m) {
    switch (op) {
    case 0: return shiftFor<0, ByCl>(s, m);
    case 1: return shiftFor<1, ByCl>(s, m);
    case 2: return shiftFor<2, ByCl>(s, m);
    case 3: return shiftFor<3, ByCl>(s, m);
    case 5: return shiftFor<5, ByCl>(s, m);
    case 7: return shiftFor<7, ByCl>(s, m);
    default: return shiftFor<4, ByCl>(s, m);
    }
}

// shld / shrd, 32- or 16-bit.
template <bool Left, unsigned S, bool M, bool ByCl> void shiftDouble(Cpu &c, const Insn &i) {
    constexpr unsigned bits = kBits<S>;
    const unsigned count = (ByCl ? c.r[kEcx] : i.imm) & 31u;
    if (count == 0 || count > bits) {
        return;
    }
    const std::uint32_t a = readRm<S, M>(c, i);
    const std::uint32_t b = getReg<S>(c, i.reg);
    std::uint64_t r;
    std::uint32_t cf;
    if constexpr (Left) {
        const std::uint64_t joined = (std::uint64_t{a} << bits) | b;
        r = (joined << count) >> bits;
        cf = static_cast<std::uint32_t>(joined >> (2 * bits - count)) & 1u;
    } else {
        const std::uint64_t joined = (std::uint64_t{b} << bits) | a;
        r = joined >> count;
        cf = static_cast<std::uint32_t>(joined >> (count - 1)) & 1u;
    }
    const std::uint32_t result = static_cast<std::uint32_t>(r) & kMask<S>;
    writeRm<S, M>(c, i, result);
    setFlagWord(c, resultWord<S>(result, cf, 0, count == 1 ? ((result ^ a) >> (bits - 1)) & 1u : 0));
}

// --- Bits. ---

// bt family: /digit 4 bt 5 bts 6 btr 7 btc. A register offset can reach past a memory operand.
template <unsigned Op, unsigned S, bool M, bool Imm> void bitTest(Cpu &c, const Insn &i) {
    constexpr unsigned bits = kBits<S>;
    const std::uint32_t offset = Imm ? i.imm : getReg<S>(c, i.reg);
    std::uint32_t va = 0;
    std::uint32_t v;
    unsigned bit;
    if constexpr (M) {
        va = effectiveAddress(c, i);
        if constexpr (Imm) {
            bit = offset & (bits - 1);
        } else {
            const std::int32_t signedOffset = S == 4 ? static_cast<std::int32_t>(offset)
                                                     : static_cast<std::int16_t>(offset);
            va += static_cast<std::uint32_t>((signedOffset >> (S == 4 ? 5 : 4)) * static_cast<std::int32_t>(S));
            bit = static_cast<std::uint32_t>(signedOffset) & (bits - 1);
        }
        v = loadN<S>(c, va);
    } else {
        bit = offset & (bits - 1);
        v = getReg<S>(c, i.rm);
    }
    c.kind = kFlagsBt;
    c.a = v;
    c.b = bit;
    c.carry = (v >> bit) & 1u;
    if constexpr (Op != 4) {
        const std::uint32_t mask = 1u << bit;
        const std::uint32_t r = Op == 5 ? v | mask : Op == 6 ? v & ~mask : v ^ mask;
        if constexpr (M) {
            storeN<S>(c, va, r);
        } else {
            setReg<S>(c, i.rm, r);
        }
    }
}

template <bool Imm> Handler bitHandler(unsigned op, unsigned s, bool m) {
#define BT4(O) (m ? &bitTest<O, 4, true, Imm> : &bitTest<O, 4, false, Imm>)
#define BT2(O) (m ? &bitTest<O, 2, true, Imm> : &bitTest<O, 2, false, Imm>)
    switch (op) {
    case 5: return s == 2 ? BT2(5) : BT4(5);
    case 6: return s == 2 ? BT2(6) : BT4(6);
    case 7: return s == 2 ? BT2(7) : BT4(7);
    default: return s == 2 ? BT2(4) : BT4(4);
    }
#undef BT4
#undef BT2
}

template <bool Reverse, unsigned S, bool M> void bitScan(Cpu &c, const Insn &i) {
    const std::uint32_t v = readRm<S, M>(c, i);
    if (v != 0) {
        setReg<S>(c, i.reg, Reverse ? 31u - static_cast<std::uint32_t>(__builtin_clz(v))
                                    : static_cast<std::uint32_t>(__builtin_ctz(v)));
    }
    c.kind = kFlagsTest;
    c.a = v;
    c.b = v;
    c.carry = 0;
}

void bswap(Cpu &c, const Insn &i) { c.r[i.reg] = __builtin_bswap32(c.r[i.reg]); }

template <unsigned S, bool M> void xadd(Cpu &c, const Insn &i) {
    const std::uint32_t a = readRm<S, M>(c, i);
    const std::uint32_t b = getReg<S>(c, i.reg);
    const std::uint32_t sum = alu<0, S>(c, a, b);
    setReg<S>(c, i.reg, a);
    writeRm<S, M>(c, i, sum);
}

template <unsigned S, bool M> void cmpxchg(Cpu &c, const Insn &i) {
    const std::uint32_t d = readRm<S, M>(c, i);
    const std::uint32_t acc = getReg<S>(c, kEax);
    alu<7, S>(c, acc, d);
    if (acc == d) {
        writeRm<S, M>(c, i, getReg<S>(c, i.reg));
    } else {
        setReg<S>(c, kEax, d);
    }
}

// --- Conditions. ---

template <unsigned S, bool M> void setcc(Cpu &c, const Insn &i) { writeRm<1, M>(c, i, condition(c, i.op) ? 1u : 0u); }
template <unsigned S, bool M> void cmovcc(Cpu &c, const Insn &i) {
    const std::uint32_t v = readRm<S, M>(c, i); // the source is read whether or not it moves
    if (condition(c, i.op)) {
        setReg<S>(c, i.reg, v);
    }
}

// --- Accumulator and flag odds and ends. ---

void cdq(Cpu &c, const Insn &) { c.r[kEdx] = static_cast<std::int32_t>(c.r[kEax]) < 0 ? 0xFFFFFFFFu : 0u; }
void cwd(Cpu &c, const Insn &) { setReg<2>(c, kEdx, static_cast<std::int16_t>(c.r[kEax]) < 0 ? 0xFFFFu : 0u); }
void cwde(Cpu &c, const Insn &) { c.r[kEax] = static_cast<std::uint32_t>(static_cast<std::int16_t>(c.r[kEax])); }
void cbw(Cpu &c, const Insn &) { setReg<2>(c, kEax, static_cast<std::uint32_t>(static_cast<std::int8_t>(c.r[kEax]))); }
void cld(Cpu &c, const Insn &) { c.direction = 1; }
void stdf(Cpu &c, const Insn &) { c.direction = -1; }
void setCarry(Cpu &c, std::uint32_t cf) { setFlagWord(c, (eflagsOf(c) & ~0x1u) | cf); }
void clc(Cpu &c, const Insn &) { setCarry(c, 0); }
void stc(Cpu &c, const Insn &) { setCarry(c, 1); }
void cmc(Cpu &c, const Insn &) { setCarry(c, (eflagsOf(c) & 1u) ^ 1u); }
void lahf(Cpu &c, const Insn &) { setReg<1>(c, 4, (eflagsOf(c) & 0xD5u) | 2u); }
// sahf: SF, ZF, AF, PF and CF from ah; OF and the rest stay as they were.
void sahf(Cpu &c, const Insn &) { setFlagWord(c, (eflagsOf(c) & ~0xD5u) | ((c.r[kEax] >> 8) & 0xD5u)); }
void xlat(Cpu &c, const Insn &i) {
    setReg<1>(c, kEax, c.load<std::uint8_t>(c.segment[i.seg] + c.r[kEbx] + (c.r[kEax] & 0xFFu)));
}
// cpuid leaves the registers as they were, on every host, as lifted code does on every host but
// an MSVC x86 build: a script's C runtime then finds no vendor it knows, reports no MMX, SSE or
// 3DNow!, and takes its x87 paths, the same on macOS, Linux and Windows.
void cpuid(Cpu &, const Insn &) {}
void rdtsc(Cpu &c, const Insn &) {
    const auto t = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    c.r[kEax] = static_cast<std::uint32_t>(t);
    c.r[kEdx] = static_cast<std::uint32_t>(t >> 32);
}

// --- Strings: movs stos lods cmps scas, with rep / repe / repne. ---

// A rep's iterations count as back edges, in batches, so a long one hands the machine over as a
// loop does (Interpreter::backEdges). An access past the guest's end ends it (Cpu::strayed).
class RepTurns {
public:
    explicit RepTurns(Cpu &c) : c_(c) {}
    // After each iteration: whether the rep goes on.
    bool turn() {
        if (++n_ == kRepBatch) {
            c_.interpreter->backEdges(c_, n_);
            n_ = 0;
        }
        return c_.straySize == 0;
    }
    void done() {
        if (n_ != 0) {
            c_.interpreter->backEdges(c_, n_);
            n_ = 0;
        }
    }

private:
    static constexpr std::uint32_t kRepBatch = 0x1000;
    Cpu &c_;
    std::uint32_t n_ = 0;
};

template <unsigned S> void movs(Cpu &c, const Insn &i) {
    const std::int32_t step = c.direction * static_cast<std::int32_t>(S);
    if (i.rep == 0) {
        storeN<S>(c, c.r[kEdi], loadN<S>(c, c.segment[i.seg] + c.r[kEsi]));
        c.r[kEsi] += step;
        c.r[kEdi] += step;
        return;
    }
    std::uint32_t n = c.r[kEcx];
    if (step > 0 && i.seg == 0) {
        // Forward copies byte by byte from the source as written: overlapping runs repeat a
        // pattern, as on the machine (memmove would not). A run past the guest's end copies nothing.
        const std::uint64_t bytes = std::uint64_t{n} * S;
        std::uint8_t *dst = c.at(c.r[kEdi], bytes);
        const std::uint8_t *src = c.at(c.r[kEsi], bytes);
        if (dst == nullptr || src == nullptr) {
            return;
        }
        if (dst + bytes <= src || src + bytes <= dst) {
            std::memcpy(dst, src, static_cast<std::size_t>(bytes));
        } else {
            for (std::uint64_t k = 0; k < bytes; ++k) {
                dst[k] = src[k];
            }
        }
        c.r[kEsi] += n * S;
        c.r[kEdi] += n * S;
        c.r[kEcx] = 0;
        return;
    }
    RepTurns turns(c);
    for (; n != 0; --n) {
        storeN<S>(c, c.r[kEdi], loadN<S>(c, c.segment[i.seg] + c.r[kEsi]));
        c.r[kEsi] += step;
        c.r[kEdi] += step;
        if (!turns.turn()) {
            break;
        }
    }
    turns.done();
    c.r[kEcx] = 0;
}

template <unsigned S> void stos(Cpu &c, const Insn &i) {
    const std::int32_t step = c.direction * static_cast<std::int32_t>(S);
    const std::uint32_t v = getReg<S>(c, kEax);
    if (i.rep == 0) {
        storeN<S>(c, c.r[kEdi], v);
        c.r[kEdi] += step;
        return;
    }
    std::uint32_t n = c.r[kEcx];
    if (step > 0 && S == 1) {
        std::uint8_t *dst = c.at(c.r[kEdi], n);
        if (dst == nullptr) {
            return;
        }
        std::memset(dst, static_cast<int>(v), n);
        c.r[kEdi] += n;
    } else {
        RepTurns turns(c);
        for (; n != 0; --n) {
            storeN<S>(c, c.r[kEdi], v);
            c.r[kEdi] += step;
            if (!turns.turn()) {
                break;
            }
        }
        turns.done();
    }
    c.r[kEcx] = 0;
}

template <unsigned S> void lods(Cpu &c, const Insn &i) {
    const std::int32_t step = c.direction * static_cast<std::int32_t>(S);
    if (i.rep == 0) {
        setReg<S>(c, kEax, loadN<S>(c, c.segment[i.seg] + c.r[kEsi]));
        c.r[kEsi] += step;
        return;
    }
    RepTurns turns(c);
    for (std::uint32_t n = c.r[kEcx]; n != 0; --n) {
        setReg<S>(c, kEax, loadN<S>(c, c.segment[i.seg] + c.r[kEsi]));
        c.r[kEsi] += step;
        if (!turns.turn()) {
            break;
        }
    }
    turns.done();
    c.r[kEcx] = 0;
}

// cmps compares [esi] with [edi]; scas compares the accumulator with [edi].
template <unsigned S, bool Scan> void compareString(Cpu &c, const Insn &i) {
    const std::int32_t step = c.direction * static_cast<std::int32_t>(S);
    auto once = [&] {
        const std::uint32_t a = Scan ? getReg<S>(c, kEax) : loadN<S>(c, c.segment[i.seg] + c.r[kEsi]);
        const std::uint32_t b = loadN<S>(c, c.r[kEdi]);
        alu<7, S>(c, a, b);
        if constexpr (!Scan) {
            c.r[kEsi] += step;
        }
        c.r[kEdi] += step;
        return a == b;
    };
    if (i.rep == 0) {
        once();
        return;
    }
    RepTurns turns(c);
    while (c.r[kEcx] != 0) {
        const bool equal = once();
        --c.r[kEcx];
        if (equal != (i.rep == 1) || !turns.turn()) { // repe stops on a difference, repne on a match
            break;
        }
    }
    turns.done();
}

// --- Control. ---

void jumpTo(Cpu &c, const Insn &i, std::uint32_t target) {
    c.next = target;
    if (target <= i.addr) {
        c.interpreter->backEdge(c);
    }
}

template <unsigned Cc> void jcc(Cpu &c, const Insn &i) {
    if (condition(c, Cc)) {
        jumpTo(c, i, i.target);
    }
}
template <unsigned Cc> void jccOut(Cpu &c, const Insn &i) {
    if (condition(c, Cc)) {
        c.interpreter->jumpOut(c, i.target);
    }
}
Handler jccHandler(unsigned cc, bool out) {
#define J(n) (out ? &jccOut<n> : &jcc<n>)
    switch (cc) {
    case 0x0: return J(0x0);
    case 0x1: return J(0x1);
    case 0x2: return J(0x2);
    case 0x3: return J(0x3);
    case 0x4: return J(0x4);
    case 0x5: return J(0x5);
    case 0x6: return J(0x6);
    case 0x7: return J(0x7);
    case 0x8: return J(0x8);
    case 0x9: return J(0x9);
    case 0xA: return J(0xA);
    case 0xB: return J(0xB);
    case 0xC: return J(0xC);
    case 0xD: return J(0xD);
    case 0xE: return J(0xE);
    default: return J(0xF);
    }
#undef J
}

void jmp(Cpu &c, const Insn &i) { jumpTo(c, i, i.target); }
void jmpOut(Cpu &c, const Insn &i) { c.interpreter->jumpOut(c, i.target); }
template <bool M> void jmpRm(Cpu &c, const Insn &i) {
    const std::uint32_t target = readRm<4, M>(c, i);
    if (c.interpreter->interprets(target)) {
        jumpTo(c, i, target);
    } else {
        c.interpreter->jumpOut(c, target);
    }
}

// A call to code at or before its own counts as a back edge, as a jump there does: recursion
// hands the machine over as a loop does.
void callIn(Cpu &c, const Insn &i) {
    c.push(i.next);
    c.next = i.target;
    if (i.target <= i.addr) {
        c.interpreter->backEdge(c);
    }
}
void callOutDirect(Cpu &c, const Insn &i) {
    c.push(i.next);
    c.interpreter->callOut(c, i.target);
}
template <bool M> void callRm(Cpu &c, const Insn &i) {
    const std::uint32_t target = readRm<4, M>(c, i);
    c.push(i.next);
    if (c.interpreter->interprets(target)) {
        c.next = target;
        if (target <= i.addr) {
            c.interpreter->backEdge(c);
        }
    } else {
        c.interpreter->callOut(c, target);
    }
}

void ret(Cpu &c, const Insn &i) {
    const bool leaving = c.r[kEsp] == c.entryEsp;
    const std::uint32_t to = c.pop();
    c.r[kEsp] += i.imm;
    if (leaving) {
        c.leave = true;
    } else {
        c.next = to;
    }
}

void jecxz(Cpu &c, const Insn &i) {
    if (c.r[kEcx] == 0) {
        jumpTo(c, i, i.target);
    }
}
// loop, loope, loopne: the condition is read before ecx counts down.
template <unsigned Kind> void loop(Cpu &c, const Insn &i) {
    const bool zf = Kind == 0 ? true : condition(c, 0x4);
    const bool go = --c.r[kEcx] != 0 && (Kind == 0 || (Kind == 1 ? zf : !zf));
    if (go) {
        jumpTo(c, i, i.target);
    }
}

// What the script cannot go on from: it stops, and the call the game made into it fails
// (Interpreter::stopScript).
void trap(Cpu &c, const Insn &i) { c.interpreter->stopScript(c, i.addr, "int3, hlt or ud2"); }
void unknown(Cpu &c, const Insn &i) { c.interpreter->stopScript(c, i.addr, "an instruction the interpreter does not know"); }
void absentExtension(Cpu &c, const Insn &i) {
    char what[32];
    std::snprintf(what, sizeof what, "%s instruction", extensionArticleName(static_cast<Extension>(i.op)));
    c.interpreter->stopScript(c, i.addr, what, " although the processor reports none");
}

// --- The decoder. ---

class Reader {
public:
    Reader(const std::uint8_t *code, std::uint32_t address) : code_(code), address_(address) {}
    std::uint8_t u8() { return code_[pos_++]; }
    std::uint32_t s8() { return static_cast<std::uint32_t>(static_cast<std::int8_t>(u8())); }
    std::uint32_t u16() {
        std::uint16_t v;
        std::memcpy(&v, code_ + pos_, 2);
        pos_ += 2;
        return v;
    }
    std::uint32_t u32() {
        std::uint32_t v;
        std::memcpy(&v, code_ + pos_, 4);
        pos_ += 4;
        return v;
    }
    std::uint32_t imm(unsigned size) { return size == 1 ? u8() : size == 2 ? u16() : u32(); }
    std::uint32_t here() const { return address_ + pos_; }
    unsigned pos() const { return pos_; }

private:
    const std::uint8_t *code_;
    std::uint32_t address_;
    unsigned pos_ = 0;
};

// Reads ModRM (and SIB, displacement) into i: reg, and rm or the memory operand.
void modrm(Reader &in, Insn &i) {
    const std::uint8_t m = in.u8();
    const unsigned mod = m >> 6;
    i.reg = (m >> 3) & 7u;
    const unsigned rm = m & 7u;
    i.op = i.reg;
    if (mod == 3) {
        i.rm = static_cast<std::uint8_t>(rm);
        return;
    }
    i.rm = kMem;
    i.base = static_cast<std::uint8_t>(rm);
    i.index = kNoReg;
    if (rm == 4) {
        const std::uint8_t sib = in.u8();
        i.scale = sib >> 6;
        const unsigned index = (sib >> 3) & 7u;
        i.index = index == 4 ? kNoReg : static_cast<std::uint8_t>(index);
        i.base = sib & 7u;
        if (i.base == 5 && mod == 0) {
            i.base = kNoReg;
            i.disp = in.u32();
        }
    } else if (rm == 5 && mod == 0) {
        i.base = kNoReg;
        i.disp = in.u32();
    }
    if (mod == 1) {
        i.disp = in.s8();
    } else if (mod == 2) {
        i.disp = in.u32();
    }
}

bool mem(const Insn &i) { return i.rm == kMem; }

} // namespace

bool decodeInstruction(const Interpreter &interpreter, const std::uint8_t *code, std::uint32_t address,
                       Insn &i) {
    Reader in(code, address);
    i = Insn{};
    i.addr = address;
    unsigned opsize = 4;
    bool known = true;
    for (;;) {
        const std::uint8_t p = code[in.pos()];
        if (p == 0x66) {
            opsize = 2;
        } else if (p == 0xF3) {
            i.rep = 1;
        } else if (p == 0xF2) {
            i.rep = 2;
        } else if (p == 0x64) {
            i.seg = 1;
        } else if (p == 0x65) {
            i.seg = 2;
        } else if (p == 0x2E || p == 0x36 || p == 0x3E || p == 0x26 || p == 0xF0) {
            // segment overrides that mean nothing in flat memory; lock: the machine is held
        } else {
            break;
        }
        in.u8();
        if (in.pos() > 4) {
            break;
        }
    }
    const std::uint8_t op = in.u8();
    auto finish = [&] {
        i.next = in.here();
        i.length = static_cast<std::uint8_t>(in.pos());
    };
    auto relative = [&](std::uint32_t displacement, bool call) {
        finish();
        i.target = i.next + displacement;
        i.endsBlock = true;
        const bool inside = interpreter.interprets(i.target);
        if (call) {
            i.run = inside ? &callIn : &callOutDirect;
        }
        return inside;
    };

    if (op < 0x40 && (op & 7u) < 6) {
        // add or adc sbb and sub xor cmp: r/m,r  r,r/m  al/eax,imm
        const unsigned alu = op >> 3;
        const unsigned form = op & 7u;
        const unsigned s = (form & 1u) != 0 ? opsize : 1;
        if (form < 4) {
            modrm(in, i);
            i.run = aluHandler(alu, form < 2 ? Form::RmReg : Form::RegRm, s, mem(i));
        } else {
            i.rm = kEax;
            i.imm = in.imm(s);
            i.run = aluHandler(alu, Form::RmImm, s, false);
        }
        i.size = static_cast<std::uint8_t>(s);
        finish();
        return true;
    }

    switch (op) {
    case 0x0F:
        break; // two-byte opcodes below
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        i.rm = op & 7u;
        i.run = opsize == 2 ? &incRm<2, false> : &incRm<4, false>;
        break;
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        i.rm = op & 7u;
        i.run = opsize == 2 ? &decRm<2, false> : &decRm<4, false>;
        break;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        i.reg = op & 7u;
        i.run = &pushReg;
        known = opsize == 4;
        break;
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        i.reg = op & 7u;
        i.run = &popReg;
        known = opsize == 4;
        break;
    case 0x60: i.run = &pushad; known = opsize == 4; break;
    case 0x61: i.run = &popad; known = opsize == 4; break;
    case 0x68: i.imm = in.u32(); i.run = &pushImm; known = opsize == 4; break;
    case 0x6A: i.imm = in.s8(); i.run = &pushImm; known = opsize == 4; break;
    case 0x69: case 0x6B:
        modrm(in, i);
        i.imm = op == 0x69 ? (opsize == 2 ? static_cast<std::uint32_t>(static_cast<std::int16_t>(in.u16())) : in.u32())
                           : in.s8();
        i.run = opsize == 2 ? (mem(i) ? &imulReg<2, true, true> : &imulReg<2, false, true>)
                            : (mem(i) ? &imulReg<4, true, true> : &imulReg<4, false, true>);
        break;
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        const std::uint32_t d = in.s8();
        const bool inside = relative(d, false);
        i.run = jccHandler(op & 0xFu, !inside);
        return true;
    }
    case 0x80: case 0x81: case 0x82: case 0x83: {
        const unsigned s = (op & 1u) != 0 ? opsize : 1;
        modrm(in, i);
        i.imm = op == 0x83 ? in.s8() : in.imm(s);
        i.run = aluHandler(i.op, Form::RmImm, s, mem(i));
        break;
    }
    case 0x84: case 0x85: {
        const unsigned s = op == 0x85 ? opsize : 1;
        modrm(in, i);
        i.run = aluHandler(8, Form::RmReg, s, mem(i));
        break;
    }
    case 0x86: case 0x87: {
        const unsigned s = op == 0x87 ? opsize : 1;
        modrm(in, i);
#define F(S, M) &xchg<S, M>
        i.run = SIZED(F, s, mem(i));
#undef F
        break;
    }
    case 0x88: case 0x89: case 0x8A: case 0x8B: {
        const unsigned s = (op & 1u) != 0 ? opsize : 1;
        modrm(in, i);
        if (op < 0x8A) {
#define F(S, M) &movRmReg<S, M>
            i.run = SIZED(F, s, mem(i));
#undef F
        } else {
#define F(S, M) &movRegRm<S, M>
            i.run = SIZED(F, s, mem(i));
#undef F
        }
        break;
    }
    case 0x8D:
        modrm(in, i);
        known = mem(i);
        i.run = opsize == 2 ? &lea<2> : &lea<4>;
        break;
    case 0x8F:
        modrm(in, i);
        known = i.op == 0 && opsize == 4;
        i.run = mem(i) ? &popRm<true> : &popRm<false>;
        break;
    case 0x90:
        i.run = &nop; // xchg eax, eax; with f3 it is pause
        break;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        i.rm = op & 7u;
        i.reg = kEax;
        i.run = opsize == 2 ? &xchg<2, false> : &xchg<4, false>;
        break;
    case 0x98: i.run = opsize == 2 ? &cbw : &cwde; break;
    case 0x99: i.run = opsize == 2 ? &cwd : &cdq; break;
    case 0x9B: i.run = &nop; break; // fwait
    case 0x9C: i.run = &pushfd; known = opsize == 4; break;
    case 0x9D: i.run = &popfd; known = opsize == 4; break;
    case 0x9E: i.run = &sahf; break;
    case 0x9F: i.run = &lahf; break;
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: {
        const unsigned s = (op & 1u) != 0 ? opsize : 1;
        i.rm = kMem;
        i.base = kNoReg;
        i.disp = in.u32();
        i.reg = kEax;
        if (op < 0xA2) {
            i.run = s == 1 ? &movRegRm<1, true> : s == 2 ? &movRegRm<2, true> : &movRegRm<4, true>;
        } else {
            i.run = s == 1 ? &movRmReg<1, true> : s == 2 ? &movRmReg<2, true> : &movRmReg<4, true>;
        }
        break;
    }
    case 0xA4: case 0xA5: {
        const unsigned s = op == 0xA5 ? opsize : 1;
        i.run = s == 1 ? &movs<1> : s == 2 ? &movs<2> : &movs<4>;
        break;
    }
    case 0xA6: case 0xA7: {
        const unsigned s = op == 0xA7 ? opsize : 1;
        i.run = s == 1 ? &compareString<1, false> : s == 2 ? &compareString<2, false> : &compareString<4, false>;
        break;
    }
    case 0xA8: case 0xA9: {
        const unsigned s = op == 0xA9 ? opsize : 1;
        i.rm = kEax;
        i.imm = in.imm(s);
        i.run = aluHandler(8, Form::RmImm, s, false);
        break;
    }
    case 0xAA: case 0xAB: {
        const unsigned s = op == 0xAB ? opsize : 1;
        i.run = s == 1 ? &stos<1> : s == 2 ? &stos<2> : &stos<4>;
        break;
    }
    case 0xAC: case 0xAD: {
        const unsigned s = op == 0xAD ? opsize : 1;
        i.run = s == 1 ? &lods<1> : s == 2 ? &lods<2> : &lods<4>;
        break;
    }
    case 0xAE: case 0xAF: {
        const unsigned s = op == 0xAF ? opsize : 1;
        i.run = s == 1 ? &compareString<1, true> : s == 2 ? &compareString<2, true> : &compareString<4, true>;
        break;
    }
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        i.rm = op & 7u;
        i.imm = in.u8();
        i.run = &movRmImm<1, false>;
        break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        i.rm = op & 7u;
        i.imm = in.imm(opsize);
        i.run = opsize == 2 ? &movRmImm<2, false> : &movRmImm<4, false>;
        break;
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        const unsigned s = (op & 1u) != 0 ? opsize : 1;
        modrm(in, i);
        if (op <= 0xC1) {
            i.imm = in.u8();
        } else {
            i.imm = 1;
        }
        i.run = op >= 0xD2 ? shiftHandler<true>(i.op, s, mem(i)) : shiftHandler<false>(i.op, s, mem(i));
        break;
    }
    case 0xC2:
        i.imm = in.u16();
        i.run = &ret;
        i.endsBlock = true;
        break;
    case 0xC3:
        i.run = &ret;
        i.endsBlock = true;
        break;
    case 0xC6: case 0xC7: {
        const unsigned s = op == 0xC7 ? opsize : 1;
        modrm(in, i);
        i.imm = in.imm(s);
        known = i.op == 0;
#define F(S, M) &movRmImm<S, M>
        i.run = SIZED(F, s, mem(i));
#undef F
        break;
    }
    case 0xC8:
        i.imm = in.u16();
        i.imm2 = in.u8();
        i.run = &enter;
        known = i.imm2 == 0;
        break;
    case 0xC9: i.run = &leave; break;
    case 0xCC: case 0xF4:
        i.run = &trap;
        i.endsBlock = true;
        break;
    case 0xD7: i.run = &xlat; break;
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        modrm(in, i);
        known = decodeFloat(op, i);
        break;
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: {
        const std::uint32_t d = in.s8();
        known = relative(d, false);
        i.run = op == 0xE3 ? &jecxz : op == 0xE2 ? &loop<0> : op == 0xE1 ? &loop<1> : &loop<2>;
        if (!known) {
            i.run = &unknown;
        }
        return known;
    }
    case 0xE8: {
        const std::uint32_t d = in.u32();
        relative(d, true);
        return true;
    }
    case 0xE9: case 0xEB: {
        const std::uint32_t d = op == 0xE9 ? in.u32() : in.s8();
        i.run = relative(d, false) ? &jmp : &jmpOut;
        return true;
    }
    case 0xF5: i.run = &cmc; break;
    case 0xF6: case 0xF7: {
        const unsigned s = op == 0xF7 ? opsize : 1;
        modrm(in, i);
        const bool m = mem(i);
        switch (i.op) {
        case 0: case 1:
            i.imm = in.imm(s);
            i.run = aluHandler(8, Form::RmImm, s, m);
            break;
#define CASE(n, fn)                                                                                \
    case n: {                                                                                      \
        i.run = SIZED(fn, s, m);                                                                   \
        break;                                                                                     \
    }
#define NOT(S, M) &notRm<S, M>
#define NEG(S, M) &negRm<S, M>
#define MUL(S, M) &mulRm<S, M>
#define IMUL(S, M) &imulRm<S, M>
#define DIV(S, M) &divRm<S, M, false>
#define IDIV(S, M) &divRm<S, M, true>
            CASE(2, NOT)
            CASE(3, NEG)
            CASE(4, MUL)
            CASE(5, IMUL)
            CASE(6, DIV)
            CASE(7, IDIV)
#undef CASE
#undef NOT
#undef NEG
#undef MUL
#undef IMUL
#undef DIV
#undef IDIV
        }
        break;
    }
    case 0xF8: i.run = &clc; break;
    case 0xF9: i.run = &stc; break;
    case 0xFC: i.run = &cld; break;
    case 0xFD: i.run = &stdf; break;
    case 0xFE: case 0xFF: {
        const unsigned s = op == 0xFF ? opsize : 1;
        modrm(in, i);
        const bool m = mem(i);
        switch (i.op) {
        case 0: {
#define F(S, M) &incRm<S, M>
            i.run = SIZED(F, s, m);
#undef F
            break;
        }
        case 1: {
#define F(S, M) &decRm<S, M>
            i.run = SIZED(F, s, m);
#undef F
            break;
        }
        case 2:
            known = op == 0xFF && opsize == 4;
            i.run = m ? &callRm<true> : &callRm<false>;
            i.endsBlock = true;
            break;
        case 4:
            known = op == 0xFF && opsize == 4;
            i.run = m ? &jmpRm<true> : &jmpRm<false>;
            i.endsBlock = true;
            break;
        case 6:
            known = op == 0xFF && opsize == 4;
            i.run = m ? &pushRm<true> : &pushRm<false>;
            break;
        default:
            known = false;
        }
        break;
    }
    default:
        known = false;
    }

    if (op == 0x0F) {
        const std::uint8_t op2 = in.u8();
        if (op2 >= 0x80 && op2 <= 0x8F) {
            const std::uint32_t d = in.u32();
            const bool inside = relative(d, false);
            i.run = jccHandler(op2 & 0xFu, !inside);
            return true;
        }
        const unsigned s = opsize;
        // MMX, 3DNow!, SSE and later: known for their length, stopping the script if it runs one.
        if (const Extension extension = extensionOf(op2, opsize == 2 || i.rep != 0); extension != Extension::kNone) {
            if (extensionHasThirdByte(op2)) {
                in.u8();
            }
            if (extensionHasModrm(op2)) {
                modrm(in, i);
            }
            if (extensionHasImmediate(op2)) {
                i.imm = in.u8();
            }
            i.op = static_cast<std::uint8_t>(extension);
            i.run = &absentExtension;
            i.endsBlock = true;
            finish();
            return true;
        }
        switch (op2) {
        case 0x0B: // ud2
            i.run = &trap;
            i.endsBlock = true;
            break;
        case 0x1F: // nop r/m
            modrm(in, i);
            i.run = &nop;
            break;
        case 0x31: i.run = &rdtsc; break;
        case 0xA2: i.run = &cpuid; break;
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
            modrm(in, i);
            i.op = op2 & 0xFu;
            i.run = s == 2 ? (mem(i) ? &cmovcc<2, true> : &cmovcc<2, false>)
                           : (mem(i) ? &cmovcc<4, true> : &cmovcc<4, false>);
            break;
        }
        case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F:
            modrm(in, i);
            i.op = op2 & 0xFu;
            i.run = mem(i) ? &setcc<1, true> : &setcc<1, false>;
            break;
        case 0xA3: case 0xAB: case 0xB3: case 0xBB:
            modrm(in, i);
            i.run = bitHandler<false>(op2 == 0xA3 ? 4 : op2 == 0xAB ? 5 : op2 == 0xB3 ? 6 : 7, s, mem(i));
            break;
        case 0xBA:
            modrm(in, i);
            i.imm = in.u8();
            known = i.op >= 4;
            i.run = bitHandler<true>(i.op, s, mem(i));
            break;
        case 0xA4: case 0xA5: case 0xAC: case 0xAD: {
            modrm(in, i);
            const bool byCl = (op2 & 1u) != 0;
            if (!byCl) {
                i.imm = in.u8();
            }
            const bool m = mem(i);
            const bool left = op2 < 0xA8;
#define SD(L, C) (s == 2 ? (m ? &shiftDouble<L, 2, true, C> : &shiftDouble<L, 2, false, C>) \
                         : (m ? &shiftDouble<L, 4, true, C> : &shiftDouble<L, 4, false, C>))
            i.run = left ? (byCl ? SD(true, true) : SD(true, false)) : (byCl ? SD(false, true) : SD(false, false));
#undef SD
            break;
        }
        case 0xAF:
            modrm(in, i);
            i.run = s == 2 ? (mem(i) ? &imulReg<2, true, false> : &imulReg<2, false, false>)
                           : (mem(i) ? &imulReg<4, true, false> : &imulReg<4, false, false>);
            break;
        case 0xB0: case 0xB1: {
            const unsigned w = op2 == 0xB1 ? s : 1;
            modrm(in, i);
#define F(S, M) &cmpxchg<S, M>
            i.run = SIZED(F, w, mem(i));
#undef F
            break;
        }
        case 0xC0: case 0xC1: {
            const unsigned w = op2 == 0xC1 ? s : 1;
            modrm(in, i);
#define F(S, M) &xadd<S, M>
            i.run = SIZED(F, w, mem(i));
#undef F
            break;
        }
        case 0xB6: case 0xB7: case 0xBE: case 0xBF: {
            modrm(in, i);
            const bool m = mem(i);
            const bool sign = op2 >= 0xBE;
            const bool word = (op2 & 1u) != 0;
#define EXT(F, G, S) (m ? &movExtend<F, G, S, true> : &movExtend<F, G, S, false>)
            if (word) {
                i.run = sign ? EXT(2, true, 4) : EXT(2, false, 4);
            } else if (s == 2) {
                i.run = sign ? EXT(1, true, 2) : EXT(1, false, 2);
            } else {
                i.run = sign ? EXT(1, true, 4) : EXT(1, false, 4);
            }
#undef EXT
            break;
        }
        case 0xBC: case 0xBD:
            modrm(in, i);
            i.run = s == 2 ? (mem(i) ? (op2 == 0xBD ? &bitScan<true, 2, true> : &bitScan<false, 2, true>)
                                     : (op2 == 0xBD ? &bitScan<true, 2, false> : &bitScan<false, 2, false>))
                           : (mem(i) ? (op2 == 0xBD ? &bitScan<true, 4, true> : &bitScan<false, 4, true>)
                                     : (op2 == 0xBD ? &bitScan<true, 4, false> : &bitScan<false, 4, false>));
            break;
        case 0xC8: case 0xC9: case 0xCA: case 0xCB: case 0xCC: case 0xCD: case 0xCE: case 0xCF:
            i.reg = op2 & 7u;
            i.run = &bswap;
            break;
        default:
            known = false;
        }
    }

    finish();
    if (!known) {
        i.run = &unknown;
        i.endsBlock = true;
    }
    return known;
}

} // namespace sfcop::scripts::interp
