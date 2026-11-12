// One decoded instruction, as the interpreter keeps it: the handler that runs it, chosen when it
// is decoded, and its operands, already taken apart. A block is a run of these ending at the first
// instruction that can change where control goes (Interpreter.h).
//
// The handlers are templates over the operand size and over whether the r/m operand is memory,
// so a handler does no decoding and almost no branching of its own.
#pragma once

#include "app/scripts/interp/Cpu.h"

#include <cstdint>

namespace sfcop::scripts::interp {

struct Insn;
using Handler = void (*)(Cpu &, const Insn &);

constexpr std::uint8_t kMem = 0xFF; // Insn::rm when the r/m operand is memory

struct Insn {
    Handler run = nullptr;
    std::uint32_t addr = 0;   // this instruction
    std::uint32_t next = 0;   // the one after it
    std::uint32_t disp = 0;   // the memory operand's displacement
    std::uint32_t imm = 0;    // the immediate, sign-extended to 32 bits when the encoding says so
    std::uint32_t target = 0; // a relative branch's destination; enter's level in imm2
    std::uint16_t imm2 = 0;
    std::uint8_t base = kNoReg; // the memory operand: segment base + base + (index << scale) + disp
    std::uint8_t index = kNoReg;
    std::uint8_t scale = 0;
    std::uint8_t seg = 0;  // 0 none, 1 fs, 2 gs
    std::uint8_t reg = 0;  // the ModRM reg field, or the register an opcode names
    std::uint8_t rm = kMem; // the ModRM r/m register, or kMem
    std::uint8_t op = 0;   // the sub-operation: the ALU op, the condition, the group's /digit
    std::uint8_t size = 4; // operand size in bytes
    std::uint8_t rep = 0;  // 0 none, 1 rep / repe, 2 repne
    std::uint8_t length = 0;
    bool endsBlock = false; // it can send control elsewhere than `next`
};

inline std::uint32_t effectiveAddress(const Cpu &c, const Insn &i) {
    return c.segment[i.seg] + c.r[i.base] + (c.r[i.index] << i.scale) + i.disp;
}

// Registers by number at a width: 8-bit numbers 4-7 are ah, ch, dh, bh.
template <unsigned S> inline std::uint32_t getReg(const Cpu &c, unsigned n) {
    if constexpr (S == 4) {
        return c.r[n];
    } else if constexpr (S == 2) {
        return c.r[n] & 0xFFFFu;
    } else {
        return n < 4 ? c.r[n] & 0xFFu : (c.r[n - 4] >> 8) & 0xFFu;
    }
}

template <unsigned S> inline void setReg(Cpu &c, unsigned n, std::uint32_t v) {
    if constexpr (S == 4) {
        c.r[n] = v;
    } else if constexpr (S == 2) {
        c.r[n] = (c.r[n] & 0xFFFF0000u) | (v & 0xFFFFu);
    } else if (n < 4) {
        c.r[n] = (c.r[n] & 0xFFFFFF00u) | (v & 0xFFu);
    } else {
        c.r[n - 4] = (c.r[n - 4] & 0xFFFF00FFu) | ((v & 0xFFu) << 8);
    }
}

template <unsigned S> inline std::uint32_t loadN(const Cpu &c, std::uint32_t va) {
    if constexpr (S == 4) {
        return c.load<std::uint32_t>(va);
    } else if constexpr (S == 2) {
        return c.load<std::uint16_t>(va);
    } else {
        return c.load<std::uint8_t>(va);
    }
}

template <unsigned S> inline void storeN(const Cpu &c, std::uint32_t va, std::uint32_t v) {
    if constexpr (S == 4) {
        c.store<std::uint32_t>(va, v);
    } else if constexpr (S == 2) {
        c.store<std::uint16_t>(va, static_cast<std::uint16_t>(v));
    } else {
        c.store<std::uint8_t>(va, static_cast<std::uint8_t>(v));
    }
}

template <unsigned S, bool M> inline std::uint32_t readRm(const Cpu &c, const Insn &i) {
    if constexpr (M) {
        return loadN<S>(c, effectiveAddress(c, i));
    } else {
        return getReg<S>(c, i.rm);
    }
}

template <unsigned S, bool M> inline void writeRm(Cpu &c, const Insn &i, std::uint32_t v) {
    if constexpr (M) {
        storeN<S>(c, effectiveAddress(c, i), v);
    } else {
        setReg<S>(c, i.rm, v);
    }
}

template <unsigned S> constexpr std::uint32_t kMask = S == 4 ? 0xFFFFFFFFu : S == 2 ? 0xFFFFu : 0xFFu;
template <unsigned S> constexpr unsigned kAlign = S == 4 ? 0 : S == 2 ? 16 : 24;
template <unsigned S> constexpr unsigned kBits = S * 8;

// The lazy flags of an instruction of width S, its operands left-aligned as the lifter keeps them.
template <unsigned S> inline void setFlags(Cpu &c, std::uint32_t kind, std::uint32_t a, std::uint32_t b) {
    c.kind = S == 4 ? kind : narrowKind(kind, kAlign<S>);
    c.a = a << kAlign<S>;
    c.b = b << kAlign<S>;
}

// The flags as a finished word, for what the triple cannot say (adc, sbb, shifts, multiplies).
inline void setFlagWord(Cpu &c, std::uint32_t word) {
    c.kind = kFlagsWord;
    c.a = word;
    c.b = 0;
    c.carry = word & 1u;
}

// The word for a result r of width S with the given carry, adjust and overflow flags.
template <unsigned S>
inline std::uint32_t resultWord(std::uint32_t r, std::uint32_t cf, std::uint32_t af, std::uint32_t of) {
    const std::uint32_t e = packFlags((r & kMask<S>) << kAlign<S>, cf, af, of);
    return (e & ~4u) | (parity8(r) << 2);
}

} // namespace sfcop::scripts::interp
