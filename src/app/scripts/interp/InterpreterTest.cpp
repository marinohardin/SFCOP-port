// The interpreter on a machine of the test's own: each case assembles a few instructions by hand,
// enters them as lifted code would (its return address on the guest stack), and checks the
// registers, flags and memory against values worked out by hand.
#include "app/scripts/interp/Interpreter.h"

#include <doctest/doctest.h>

#include <csetjmp>
#include <cstdint>
#include <cstring>
#include <condition_variable>
#include <initializer_list>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace sfcop::scripts::interp;

namespace {

constexpr std::uint32_t kCode = 0x1000;
constexpr std::uint32_t kStackTop = 0x80000;
constexpr std::uint32_t kHost = 0x00500000; // an address the lookup answers with a host function

struct TestMachine;
TestMachine *g_current = nullptr;

struct TestMachine {
    std::uint32_t regs[8] = {};
    std::uint32_t kind = 0, a = 0, b = 0, carry = 0;
    double st[8] = {};
    std::int64_t exact[8] = {};
    int top = 0;
    std::uint16_t cw = 0x027F;
    std::uint32_t fs = 0, gs = 0, current = 0, backEdges = 0;
    void (*yield)(void) = nullptr;
    void (*divide)(std::uint32_t) = nullptr;
    std::vector<std::uint8_t> memory = std::vector<std::uint8_t>(0x100000, 0);
    Machine machine;

    int hostCalls = 0;
    std::uint32_t hostSawReturn = 0;
    int yields = 0;
    std::uint32_t divideCode = 0;

    TestMachine() {
        for (unsigned n = 0; n < 8; ++n) {
            machine.regs[n] = &regs[n];
        }
        machine.flagKind = &kind;
        machine.flagA = &a;
        machine.flagB = &b;
        machine.flagCarry = &carry;
        machine.st = st;
        machine.stExact = exact;
        machine.fpTop = &top;
        machine.fpuControl = &cw;
        machine.fsBase = &fs;
        machine.gsBase = &gs;
        machine.currentFunction = &current;
        machine.memory = memory.data();
        machine.backEdges = &backEdges;
        machine.yieldHook = &yield;
        machine.divideFaultHook = &divide;
        machine.lookup = [](std::uint32_t address) -> HostFunction {
            return address == kHost ? &TestMachine::host : nullptr;
        };
        machine.fatal = [](const char *message) { throw std::runtime_error(message); };
        g_current = this;
    }

    // A lifted function: sees the call's return address on top of the stack, pops it, eax = 41.
    static void host() {
        TestMachine &t = *g_current;
        ++t.hostCalls;
        t.hostSawReturn = t.load32(t.regs[kEsp]);
        t.regs[kEsp] += 4;
        t.regs[kEax] = 41;
    }

    std::uint32_t load32(std::uint32_t va) const {
        std::uint32_t v;
        std::memcpy(&v, memory.data() + va, 4);
        return v;
    }
    void store32(std::uint32_t va, std::uint32_t v) { std::memcpy(memory.data() + va, &v, 4); }
    void put(std::uint32_t va, std::initializer_list<std::uint8_t> bytes) {
        std::uint32_t at = va;
        for (const std::uint8_t byte : bytes) {
            memory[at++] = byte;
        }
    }

    // Enters the code at kCode as a call from lifted code; checks that it returned to the caller.
    void run(Interpreter &interpreter) {
        regs[kEsp] = kStackTop - 4;
        store32(kStackTop - 4, kLiftedReturnAddress);
        interpreter.call(kCode);
        CHECK(regs[kEsp] == kStackTop);
    }
    void run(std::initializer_list<std::uint8_t> code) {
        put(kCode, code);
        Interpreter interpreter(machine);
        interpreter.addCode(kCode, kCode + 0x1000);
        run(interpreter);
    }
};

std::uint8_t low(std::uint32_t v) { return static_cast<std::uint8_t>(v); }
std::uint8_t high(std::uint32_t v) { return static_cast<std::uint8_t>(v >> 8); }

} // namespace

TEST_CASE("an add that wraps to zero leaves the add's operands as flags and sets ZF and CF") {
    TestMachine t;
    t.run({0xB8, 0x05, 0x00, 0x00, 0x00, // mov eax, 5
           0x83, 0xC0, 0xFB,             // add eax, -5
           0x0F, 0x94, 0xC1,             // sete cl
           0x0F, 0x92, 0xC5,             // setb ch
           0xC3});
    CHECK(t.regs[kEax] == 0u);
    CHECK(low(t.regs[kEcx]) == 1);
    CHECK(high(t.regs[kEcx]) == 1);
    CHECK(t.kind == kFlagsAdd);
    CHECK(t.a == 5u);
    CHECK(t.b == 0xFFFFFFFBu);
    CHECK(t.carry == 1u);
}

TEST_CASE("a counted loop sums 10 down to 1 and counts its back edges") {
    TestMachine t;
    t.backEdges = 100;
    t.run({0x31, 0xC0,                   // xor eax, eax
           0xB9, 0x0A, 0x00, 0x00, 0x00, // mov ecx, 10
           0x01, 0xC8,                   // L: add eax, ecx
           0x49,                         // dec ecx
           0x75, 0xFB,                   // jnz L
           0xC3});
    CHECK(t.regs[kEax] == 55u);
    CHECK(t.regs[kEcx] == 0u);
    CHECK(t.backEdges == 109u);
    CHECK(t.kind == kFlagsDec);
    CHECK(t.a == 1u);
    CHECK(t.b == 1u);
}

TEST_CASE("an 8-bit add keeps its operands left-aligned, the kind carrying the shift") {
    TestMachine t;
    t.run({0xB0, 0xFF,       // mov al, 0xFF
           0x04, 0x01,       // add al, 1
           0x0F, 0x92, 0xC3, // setb bl
           0x0F, 0x94, 0xC7, // sete bh
           0xC3});
    CHECK(low(t.regs[kEax]) == 0);
    CHECK(low(t.regs[kEbx]) == 1);
    CHECK(high(t.regs[kEbx]) == 1);
    CHECK(t.kind == 0x1802u);
    CHECK(t.a == 0xFF000000u);
    CHECK(t.b == 0x01000000u);
}

TEST_CASE("a compare answers signed and unsigned conditions differently") {
    TestMachine t;
    t.run({0xB8, 0xFF, 0xFF, 0xFF, 0xFF, // mov eax, -1
           0x83, 0xF8, 0x01,             // cmp eax, 1
           0x0F, 0x9C, 0xC1,             // setl cl
           0x0F, 0x92, 0xC2,             // setb dl
           0x0F, 0x97, 0xC3,             // seta bl
           0x0F, 0x9F, 0xC7,             // setg bh
           0xC3});
    CHECK(low(t.regs[kEcx]) == 1);
    CHECK(low(t.regs[kEdx]) == 0);
    CHECK(low(t.regs[kEbx]) == 1);
    CHECK(high(t.regs[kEbx]) == 0);
    CHECK(t.kind == kFlagsCmp);
    CHECK(t.carry == 0u);
}

TEST_CASE("shifts leave a flag word; sar keeps the sign") {
    TestMachine t;
    t.run({0xBA, 0xF8, 0xFF, 0xFF, 0xFF, // mov edx, -8
           0xC1, 0xFA, 0x02,             // sar edx, 2
           0xB8, 0x01, 0x00, 0x00, 0x80, // mov eax, 0x80000001
           0xD1, 0xE0,                   // shl eax, 1
           0xC3});
    CHECK(t.regs[kEdx] == 0xFFFFFFFEu);
    CHECK(t.regs[kEax] == 2u);
    CHECK(t.kind == kFlagsWord);
    CHECK(t.a == 0x00000A03u); // CF, OF (msb 0 xor CF 1), the fixed bits; 0x02 has odd parity
    CHECK(t.carry == 1u);
}

TEST_CASE("mul and idiv use edx:eax") {
    TestMachine t;
    t.run({0xB8, 0x00, 0x00, 0x01, 0x00, // mov eax, 0x10000
           0xB9, 0x00, 0x00, 0x01, 0x00, // mov ecx, 0x10000
           0xF7, 0xE1,                   // mul ecx
           0x89, 0xD3,                   // mov ebx, edx
           0xB8, 0x64, 0x00, 0x00, 0x00, // mov eax, 100
           0x99,                         // cdq
           0xB9, 0xF9, 0xFF, 0xFF, 0xFF, // mov ecx, -7
           0xF7, 0xF9,                   // idiv ecx
           0xC3});
    CHECK(t.regs[kEbx] == 1u);
    CHECK(t.regs[kEax] == 0xFFFFFFF2u); // -14
    CHECK(t.regs[kEdx] == 2u);
}

TEST_CASE("memory operands with SIB, movzx and movsx") {
    TestMachine t;
    t.run({0xBB, 0x00, 0x30, 0x00, 0x00,                   // mov ebx, 0x3000
           0xBE, 0x02, 0x00, 0x00, 0x00,                   // mov esi, 2
           0xC7, 0x44, 0xB3, 0x08, 0x44, 0x33, 0x22, 0x81, // mov dword [ebx+esi*4+8], 0x81223344
           0x0F, 0xB6, 0x43, 0x11,                         // movzx eax, byte [ebx+0x11]
           0x0F, 0xBF, 0x4B, 0x12,                         // movsx ecx, word [ebx+0x12]
           0x8D, 0x54, 0xB3, 0xFC,                         // lea edx, [ebx+esi*4-4]
           0xC3});
    CHECK(t.load32(0x3010) == 0x81223344u);
    CHECK(t.regs[kEax] == 0x33u);
    CHECK(t.regs[kEcx] == 0xFFFF8122u);
    CHECK(t.regs[kEdx] == 0x3004u);
}

TEST_CASE("a call inside the script pushes its real return address and returns to it") {
    TestMachine t;
    t.run({0xE8, 0x04, 0x00, 0x00, 0x00, // call 0x1009
           0x40,                         // inc eax
           0xC3,                         // ret
           0x90, 0x90,                   //
           0x8B, 0x0C, 0x24,             // 0x1009: mov ecx, [esp]
           0xB8, 0x07, 0x00, 0x00, 0x00, // mov eax, 7
           0xC3});
    CHECK(t.regs[kEcx] == 0x1005u);
    CHECK(t.regs[kEax] == 8u);
}

TEST_CASE("a call out goes through the lookup with registers and flags handed over") {
    TestMachine t;
    t.store32(0x2000, kHost);
    t.run({0x83, 0xF9, 0x02,                   // cmp ecx, 2
           0xFF, 0x15, 0x00, 0x20, 0x00, 0x00, // call [0x2000]
           0x05, 0x01, 0x00, 0x00, 0x00,       // add eax, 1
           0xC3});
    CHECK(t.hostCalls == 1);
    CHECK(t.hostSawReturn == 0x1009u);
    CHECK(t.regs[kEax] == 42u);
}

TEST_CASE("a tail jump out returns straight to the lifted caller") {
    TestMachine t;
    t.store32(0x2000, kHost);
    t.run({0xFF, 0x25, 0x00, 0x20, 0x00, 0x00}); // jmp [0x2000]
    CHECK(t.hostCalls == 1);
    CHECK(t.hostSawReturn == kLiftedReturnAddress);
    CHECK(t.regs[kEax] == 41u);
}

TEST_CASE("a call out that nothing answers gives eax 0 and goes on, as an unresolved lifted call") {
    TestMachine t;
    t.store32(0x2000, 0x00600000); // nothing answers it
    t.run({0xB8, 0x07, 0x00, 0x00, 0x00,       // mov eax, 7
           0xFF, 0x15, 0x00, 0x20, 0x00, 0x00, // call [0x2000]
           0x8B, 0xD8,                         // mov ebx, eax
           0xB8, 0x05, 0x00, 0x00, 0x00,       // mov eax, 5
           0xC3});
    CHECK(t.hostCalls == 0);
    CHECK(t.regs[kEbx] == 0u);
    CHECK(t.regs[kEax] == 5u);
    CHECK(t.regs[kEsp] == kStackTop); // the call's return address taken back off, the caller's popped
}

TEST_CASE("cpuid leaves the registers as they were, on every host") {
    TestMachine t;
    t.run({0xB8, 0x00, 0x00, 0x00, 0x00, // mov eax, 0
           0xBB, 0x11, 0x11, 0x11, 0x11, // mov ebx, 0x11111111
           0xB9, 0x22, 0x22, 0x22, 0x22, // mov ecx, 0x22222222
           0xBA, 0x33, 0x33, 0x33, 0x33, // mov edx, 0x33333333
           0x0F, 0xA2,                   // cpuid
           0xC3});
    CHECK(t.regs[kEax] == 0u); // leaf 0 asked: the highest leaf is 0, so a C runtime asks no more
    CHECK(t.regs[kEbx] == 0x11111111u);
    CHECK(t.regs[kEcx] == 0x22222222u);
    CHECK(t.regs[kEdx] == 0x33333333u);

    TestMachine one; // leaf 1: no feature bit appears, whatever the host has
    one.run({0xB8, 0x01, 0x00, 0x00, 0x00, // mov eax, 1
             0x31, 0xC9,                   // xor ecx, ecx
             0x31, 0xD2,                   // xor edx, edx
             0x0F, 0xA2,                   // cpuid
             0xC3});
    CHECK(one.regs[kEax] == 1u);
    CHECK(one.regs[kEcx] == 0u);
    CHECK(one.regs[kEdx] == 0u);
}

TEST_CASE("flags cross the entry: a branch reads the lifted caller's compare") {
    TestMachine t;
    t.kind = kFlagsCmp;
    t.a = 1;
    t.b = 2;
    t.run({0x0F, 0x92, 0xC0, 0xC3}); // setb al
    CHECK(low(t.regs[kEax]) == 1);
}

TEST_CASE("repne scasb measures a string and rep stosd fills") {
    TestMachine t;
    t.put(0x3000, {'a', 'b', 'c', 0});
    t.run({0xBF, 0x00, 0x30, 0x00, 0x00, // mov edi, 0x3000
           0x31, 0xC0,                   // xor eax, eax
           0xB9, 0xFF, 0xFF, 0xFF, 0xFF, // mov ecx, -1
           0xF2, 0xAE,                   // repne scasb
           0xF7, 0xD1,                   // not ecx
           0x49,                         // dec ecx
           0x89, 0xCB,                   // mov ebx, ecx
           0xBF, 0x00, 0x40, 0x00, 0x00, // mov edi, 0x4000
           0xB8, 0xDD, 0xCC, 0xBB, 0xAA, // mov eax, 0xAABBCCDD
           0xB9, 0x03, 0x00, 0x00, 0x00, // mov ecx, 3
           0xF3, 0xAB,                   // rep stosd
           0xC3});
    CHECK(t.regs[kEbx] == 3u);
    CHECK(t.load32(0x4000) == 0xAABBCCDDu);
    CHECK(t.load32(0x4008) == 0xAABBCCDDu);
    CHECK(t.load32(0x400C) == 0u);
    CHECK(t.regs[kEdi] == 0x400Cu);
    CHECK(t.regs[kEcx] == 0u);
}

TEST_CASE("adc and sbb carry a word between halves") {
    TestMachine t;
    t.run({0xB8, 0xFF, 0xFF, 0xFF, 0xFF, // mov eax, -1
           0x83, 0xC0, 0x01,             // add eax, 1
           0xBA, 0x00, 0x00, 0x00, 0x00, // mov edx, 0
           0x83, 0xD2, 0x00,             // adc edx, 0
           0x31, 0xC0,                   // xor eax, eax
           0x83, 0xE8, 0x01,             // sub eax, 1
           0xBB, 0x05, 0x00, 0x00, 0x00, // mov ebx, 5
           0x83, 0xDB, 0x00,             // sbb ebx, 0
           0xC3});
    CHECK(t.regs[kEdx] == 1u);
    CHECK(t.regs[kEbx] == 4u);
}

TEST_CASE("a rotate changes CF and keeps ZF; inc keeps CF") {
    TestMachine t;
    t.run({0x31, 0xC0,       // xor eax, eax
           0xB1, 0x81,       // mov cl, 0x81
           0xD0, 0xC1,       // rol cl, 1
           0x0F, 0x94, 0xC2, // sete dl
           0x0F, 0x92, 0xC6, // setb dh
           0xF9,             // stc
           0x48,             // dec eax
           0x40,             // inc eax
           0x0F, 0x92, 0xC3, // setb bl
           0x0F, 0x94, 0xC7, // sete bh
           0xC3});
    CHECK(low(t.regs[kEcx]) == 0x03);
    CHECK(low(t.regs[kEdx]) == 1);
    CHECK(high(t.regs[kEdx]) == 1);
    CHECK(low(t.regs[kEbx]) == 1);
    CHECK(high(t.regs[kEbx]) == 1);
}

TEST_CASE("pushfd reports the compare's flags as a word") {
    TestMachine t;
    t.run({0xB8, 0x01, 0x00, 0x00, 0x00, // mov eax, 1
           0x83, 0xF8, 0x02,             // cmp eax, 2
           0x9C,                         // pushfd
           0x59,                         // pop ecx
           0xC3});
    CHECK(t.regs[kEcx] == 0x00000297u); // CF PF AF SF and the fixed bits
}

TEST_CASE("the x87 stack is the machine's: loads, arithmetic, stores and the status word") {
    TestMachine t;
    t.store32(0x3000, 0x3FC00000u); // 1.5f
    t.store32(0x3004, 0x40000000u); // 2.0f
    const double half = 0.5;
    std::memcpy(t.memory.data() + 0x3010, &half, 8);
    t.store32(0x3018, 10);
    t.regs[kEax] = 0x12340000u;
    t.run({0xD9, 0x05, 0x00, 0x30, 0x00, 0x00, // fld dword [0x3000]
           0xD9, 0x05, 0x04, 0x30, 0x00, 0x00, // fld dword [0x3004]
           0xDE, 0xC9,                         // fmulp st(1)
           0xDB, 0x1D, 0x08, 0x30, 0x00, 0x00, // fistp dword [0x3008]
           0xD9, 0xE8,                         // fld1
           0xDD, 0x05, 0x10, 0x30, 0x00, 0x00, // fld qword [0x3010]
           0xDE, 0xD9,                         // fcompp
           0xDF, 0xE0,                         // fnstsw ax
           0xDB, 0x05, 0x18, 0x30, 0x00, 0x00, // fild dword [0x3018]
           0xD9, 0x05, 0x04, 0x30, 0x00, 0x00, // fld dword [0x3004]
           0xDE, 0xE9,                         // fsubp st(1): st(1) - st(0)
           0xDD, 0x1D, 0x20, 0x30, 0x00, 0x00, // fstp qword [0x3020]
           0xC3});
    CHECK(t.load32(0x3008) == 3u);
    CHECK(t.regs[kEax] == 0x12340100u);
    double difference;
    std::memcpy(&difference, t.memory.data() + 0x3020, 8);
    CHECK(difference == 8.0);
    CHECK(t.top == 0);
}

TEST_CASE("fdivp by zero keeps the dividend, as lifted code does") {
    TestMachine t;
    t.run({0xD9, 0xE8,                         // fld1
           0xD9, 0xEE,                         // fldz
           0xDE, 0xF9,                         // fdivp st(1): st(1) / st(0)
           0xDD, 0x1D, 0x00, 0x30, 0x00, 0x00, // fstp qword [0x3000]
           0xC3});
    double v;
    std::memcpy(&v, t.memory.data() + 0x3000, 8);
    CHECK(v == 1.0);
}

TEST_CASE("the yield hook runs on every 0x10000th back edge") {
    TestMachine t;
    t.backEdges = 0xFFF8;
    t.yield = [] { ++g_current->yields; };
    t.run({0x31, 0xC0, 0xB9, 0x0A, 0x00, 0x00, 0x00, 0x01, 0xC8, 0x49, 0x75, 0xFB, 0xC3});
    CHECK(t.yields == 1);
    CHECK(t.backEdges == 0x10001u);
}

TEST_CASE("a division by zero raises through the hook and reads 0") {
    TestMachine t;
    t.divide = [](std::uint32_t code) { g_current->divideCode = code; };
    t.run({0xB8, 0x07, 0x00, 0x00, 0x00, // mov eax, 7
           0x31, 0xC9,                   // xor ecx, ecx
           0xF7, 0xF1,                   // div ecx
           0xC3});
    CHECK(t.divideCode == 0xC0000094u);
    CHECK(t.regs[kEax] == 0u);
    CHECK(t.regs[kEdx] == 0u);
}

TEST_CASE("a quotient that does not fit raises the overflow through the hook and reads 0") {
    TestMachine smallest; // edx:eax = -2^63, idiv by -1: the quotient 2^63 fits no register
    smallest.divide = [](std::uint32_t code) { g_current->divideCode = code; };
    smallest.run({0xBA, 0x00, 0x00, 0x00, 0x80, // mov edx, 0x80000000
                  0x31, 0xC0,                   // xor eax, eax
                  0xB9, 0xFF, 0xFF, 0xFF, 0xFF, // mov ecx, -1
                  0xF7, 0xF9,                   // idiv ecx
                  0xC3});
    CHECK(smallest.divideCode == 0xC0000095u);
    CHECK(smallest.regs[kEax] == 0u);
    CHECK(smallest.regs[kEdx] == 0u);

    TestMachine past; // -2^31 / -1 = 2^31, one past the largest int32
    past.divide = [](std::uint32_t code) { g_current->divideCode = code; };
    past.run({0xBA, 0xFF, 0xFF, 0xFF, 0xFF, // mov edx, -1
              0xB8, 0x00, 0x00, 0x00, 0x80, // mov eax, 0x80000000
              0xB9, 0xFF, 0xFF, 0xFF, 0xFF, // mov ecx, -1
              0xF7, 0xF9,                   // idiv ecx
              0xC3});
    CHECK(past.divideCode == 0xC0000095u);
    CHECK(past.regs[kEax] == 0u);

    TestMachine fits; // -(2^31 - 1) / -1 = 2^31 - 1 fits: no fault
    fits.divide = [](std::uint32_t code) { g_current->divideCode = code; };
    fits.run({0xBA, 0xFF, 0xFF, 0xFF, 0xFF, // mov edx, -1
              0xB8, 0x01, 0x00, 0x00, 0x80, // mov eax, 0x80000001
              0xB9, 0xFF, 0xFF, 0xFF, 0xFF, // mov ecx, -1
              0xF7, 0xF9,                   // idiv ecx
              0xC3});
    CHECK(fits.divideCode == 0u);
    CHECK(fits.regs[kEax] == 0x7FFFFFFFu);
    CHECK(fits.regs[kEdx] == 0u);

    TestMachine wide; // div: 2^32 / 1 does not fit 32 bits
    wide.divide = [](std::uint32_t code) { g_current->divideCode = code; };
    wide.run({0xBA, 0x01, 0x00, 0x00, 0x00, // mov edx, 1
              0x31, 0xC0,                   // xor eax, eax
              0xB9, 0x01, 0x00, 0x00, 0x00, // mov ecx, 1
              0xF7, 0xF1,                   // div ecx
              0xC3});
    CHECK(wide.divideCode == 0xC0000095u);
    CHECK(wide.regs[kEax] == 0u);
    CHECK(wide.regs[kEdx] == 0u);

    TestMachine byte; // idiv cl: -32768 / -1 does not fit 8 bits
    byte.divide = [](std::uint32_t code) { g_current->divideCode = code; };
    byte.regs[kEax] = 0x12340000u;
    byte.run({0x66, 0xB8, 0x00, 0x80, // mov ax, 0x8000
              0xB1, 0xFF,             // mov cl, -1
              0xF6, 0xF9,             // idiv cl
              0xC3});
    CHECK(byte.divideCode == 0xC0000095u);
    CHECK(byte.regs[kEax] == 0x12340000u); // ax 0; the upper half untouched
}

TEST_CASE("fist stores the integer indefinite for NaN and out of range, on every host") {
    TestMachine t;
    t.store32(0x3000, 0x7FC00000u); // NaN
    t.store32(0x3004, 0x501502F9u); // 1e10f
    t.store32(0x3008, 0x4F000000u); // 2^31, one past the largest int32
    t.store32(0x300C, 0xCF000000u); // -2^31, the smallest int32, in range
    t.run({0xD9, 0x05, 0x00, 0x30, 0x00, 0x00, // fld dword [0x3000]
           0xDB, 0x1D, 0x00, 0x31, 0x00, 0x00, // fistp dword [0x3100]
           0xD9, 0x05, 0x00, 0x30, 0x00, 0x00, // fld dword [0x3000]
           0xDF, 0x1D, 0x04, 0x31, 0x00, 0x00, // fistp word [0x3104]
           0xD9, 0x05, 0x00, 0x30, 0x00, 0x00, // fld dword [0x3000]
           0xDF, 0x3D, 0x08, 0x31, 0x00, 0x00, // fistp qword [0x3108]
           0xD9, 0x05, 0x04, 0x30, 0x00, 0x00, // fld dword [0x3004]
           0xDB, 0x15, 0x10, 0x31, 0x00, 0x00, // fist dword [0x3110]
           0xDD, 0xD8,                         // fstp st(0)
           0xD9, 0x05, 0x08, 0x30, 0x00, 0x00, // fld dword [0x3008]
           0xDB, 0x1D, 0x14, 0x31, 0x00, 0x00, // fistp dword [0x3114]
           0xD9, 0x05, 0x0C, 0x30, 0x00, 0x00, // fld dword [0x300C]
           0xDB, 0x1D, 0x18, 0x31, 0x00, 0x00, // fistp dword [0x3118]
           0xC3});
    CHECK(t.load32(0x3100) == 0x80000000u);
    CHECK(t.load32(0x3104) == 0x00008000u);
    CHECK(t.load32(0x3108) == 0u);
    CHECK(t.load32(0x310C) == 0x80000000u);
    CHECK(t.load32(0x3110) == 0x80000000u);
    CHECK(t.load32(0x3114) == 0x80000000u);
    CHECK(t.load32(0x3118) == 0x80000000u);
    CHECK(t.top == 0);
}

TEST_CASE("sahf takes SF, ZF, AF, PF and CF from ah and keeps OF") {
    TestMachine t;
    t.run({0xB8, 0xFF, 0xFF, 0xFF, 0x7F, // mov eax, 0x7FFFFFFF
           0x83, 0xC0, 0x01,             // add eax, 1: OF and SF set
           0xB4, 0x01,                   // mov ah, 0x01: CF only
           0x9E,                         // sahf
           0x0F, 0x90, 0xC3,             // seto bl
           0x0F, 0x92, 0xC7,             // setb bh
           0x0F, 0x98, 0xC1,             // sets cl
           0x0F, 0x94, 0xC5,             // sete ch
           0xC3});
    CHECK(low(t.regs[kEbx]) == 1);
    CHECK(high(t.regs[kEbx]) == 1);
    CHECK(low(t.regs[kEcx]) == 0);
    CHECK(high(t.regs[kEcx]) == 0);
}

TEST_CASE("blocks are decoded once and run again from the cache") {
    TestMachine t;
    t.put(kCode, {0x31, 0xC0, 0xB9, 0x0A, 0x00, 0x00, 0x00, 0x01, 0xC8, 0x49, 0x75, 0xFB, 0xC3});
    Interpreter interpreter(t.machine);
    interpreter.addCode(kCode, kCode + 0x1000);
    t.run(interpreter);
    const std::uint64_t decoded = interpreter.stats().blocksDecoded;
    CHECK(decoded == 3u);
    // xor, mov, add, dec, jne once; add, dec, jne nine times more; ret.
    CHECK(interpreter.stats().instructionsRun == 33u);
    t.run(interpreter);
    CHECK(interpreter.stats().instructionsRun == 66u);
    CHECK(interpreter.stats().blocksDecoded == decoded);
    CHECK(t.regs[kEax] == 55u);
}

namespace {

std::string g_report;
void keepReport(const char *line) { g_report = line; }

// A stdcall function of two arguments that saves ebx and esi, pushes an x87 value, and then runs
// `stop` before it would return 9.
void putStopping(TestMachine &t, std::initializer_list<std::uint8_t> stop) {
    t.put(kCode, {0x53,                         // push ebx
                  0x56,                         // push esi
                  0xBB, 0x11, 0x11, 0x11, 0x11, // mov ebx, 0x11111111
                  0xBE, 0x22, 0x22, 0x22, 0x22, // mov esi, 0x22222222
                  0xD9, 0xE8});                 // fld1
    for (std::uint32_t at = kCode + 14; at < kCode + 30; ++at) {
        t.memory[at] = 0x90; // nop
    }
    t.put(kCode + 14, stop);
    t.put(kCode + 30, {0xB8, 0x09, 0x00, 0x00, 0x00, // mov eax, 9
                       0x5E, 0x5B,                   // pop esi; pop ebx
                       0xC2, 0x08, 0x00});           // ret 8
}

// Calls the function at kCode with two arguments, as lifted stdcall code does.
void callStopping(TestMachine &t, Interpreter &interpreter) {
    t.regs[kEsp] = kStackTop - 12;
    t.store32(kStackTop - 12, kLiftedReturnAddress);
    t.regs[kEbx] = 0xB0B0B0B0u;
    t.regs[kEsi] = 0x5E5E5E5Eu;
    t.regs[kEdx] = 0xD0D0D0D0u;
    interpreter.call(kCode);
}

} // namespace

TEST_CASE("unresolved calls out are said through the report, the first 16 only") {
    TestMachine t;
    static int lines = 0;
    lines = 0;
    g_report.clear();
    t.machine.report = [](const char *line) {
        ++lines;
        g_report = line;
    };
    t.store32(0x2000, 0x00600000); // nothing answers it
    t.put(kCode, {0xB9, 0x14, 0x00, 0x00, 0x00,       // mov ecx, 20
                  0x51,                               // push ecx
                  0xFF, 0x15, 0x00, 0x20, 0x00, 0x00, // call [0x2000]
                  0x59,                               // pop ecx
                  0x49,                               // dec ecx
                  0x75, 0xF5,                         // jnz the push
                  0xC3});
    Interpreter interpreter(t.machine);
    interpreter.addCode(kCode, kCode + 0x1000);
    t.run(interpreter);
    CHECK(lines == 16);
    CHECK(interpreter.stats().unresolvedCalls == 20u);
    CHECK(g_report.rfind("[scripts] ICALL: unresolved VA 0x00600000 from 0x", 0) == 0);
    CHECK(g_report.size() > 27);
    CHECK(g_report.substr(g_report.size() - 27) == "; further ones are not said");
}

TEST_CASE("an MMX instruction stops the script: the call fails, and every later one, with nothing to undo") {
    TestMachine t;
    t.machine.report = &keepReport;
    putStopping(t, {0x0F, 0xFD, 0xC1}); // paddw mm0, mm1
    Interpreter interpreter(t.machine);
    interpreter.addCode(kCode, kCode + 0x1000, "TestScript", 0x10001000);
    callStopping(t, interpreter);
    CHECK(g_report == "[scripts] stopped: TestScript: it reached an MMX instruction at 0x1000100E (bytes 0F FD "
                      "C1 90) although the processor reports none; the call into it fails, as does every later one");
    CHECK(t.regs[kEax] == 0u);
    CHECK(t.regs[kEdx] == 0u);
    CHECK(t.regs[kEbx] == 0xB0B0B0B0u); // the caller's, not the script's
    CHECK(t.regs[kEsi] == 0x5E5E5E5Eu);
    CHECK(t.regs[kEsp] == kStackTop);   // the return address and both arguments popped, as ret 8 would
    CHECK(t.top == 0);                  // the fld1 undone
    CHECK(interpreter.stats().stops == 1u);
    CHECK(interpreter.stopped(kCode));

    g_report.clear();
    const std::uint64_t ran = interpreter.stats().instructionsRun;
    callStopping(t, interpreter);
    CHECK(g_report.empty()); // said once
    CHECK(interpreter.stats().instructionsRun == ran);
    CHECK(t.regs[kEax] == 0u);
    CHECK(t.regs[kEsp] == kStackTop);
}

TEST_CASE("3DNow! and SSE forms stop the script with their own names") {
    TestMachine t;
    t.machine.report = &keepReport;
    SUBCASE("3DNow!") {
        putStopping(t, {0x0F, 0x0F, 0xC8, 0x96}); // pfrcp mm1, mm0
        Interpreter interpreter(t.machine);
        interpreter.addCode(kCode, kCode + 0x1000, "TestScript");
        callStopping(t, interpreter);
        CHECK(g_report.find("it reached a 3DNow! instruction at 0x0000100E (bytes 0F 0F C8 96) although") !=
              std::string::npos);
    }
    SUBCASE("an MMX opcode with 66 is SSE") {
        putStopping(t, {0x66, 0x0F, 0x6F, 0xC1}); // movdqa xmm0, xmm1
        Interpreter interpreter(t.machine);
        interpreter.addCode(kCode, kCode + 0x1000, "TestScript");
        callStopping(t, interpreter);
        CHECK(g_report.find("it reached an SSE instruction at 0x0000100E (bytes 66 0F 6F C1)") != std::string::npos);
    }
    SUBCASE("shufps, with its immediate") {
        putStopping(t, {0x0F, 0xC6, 0xC1, 0x1B, 0x0F, 0x0B}); // shufps xmm0, xmm1, 0x1B; ud2
        Interpreter interpreter(t.machine);
        interpreter.addCode(kCode, kCode + 0x1000, "TestScript");
        Insn insn;
        CHECK(decodeInstruction(interpreter, t.memory.data() + kCode + 14, kCode + 14, insn));
        CHECK(insn.length == 4u);
        callStopping(t, interpreter);
        CHECK(g_report.find("an SSE instruction") != std::string::npos);
    }
    CHECK(t.regs[kEsp] == kStackTop);
    CHECK(t.regs[kEax] == 0u);
}

TEST_CASE("an instruction the interpreter does not know stops the script at run time, not the client") {
    TestMachine t;
    t.machine.report = &keepReport;
    putStopping(t, {0x0F, 0x05}); // syscall
    Interpreter interpreter(t.machine);
    interpreter.addCode(kCode, kCode + 0x1000, "TestScript");
    callStopping(t, interpreter);
    CHECK(g_report == "[scripts] stopped: TestScript: it reached an instruction the interpreter does not know at "
                      "0x0000100E (bytes 0F 05 90 90); the call into it fails, as does every later one");
    // Past an instruction it does not know, the search for the function's ret cannot go on: its
    // arguments are left on the stack, the return address popped.
    CHECK(t.regs[kEsp] == kStackTop - 8);
    CHECK(t.regs[kEbx] == 0xB0B0B0B0u);
}

TEST_CASE("a return to an address outside every script stops the script, not the client") {
    TestMachine t;
    t.machine.report = &keepReport;
    putStopping(t, {0x68, 0x00, 0x00, 0x70, 0x00, // push 0x00700000, a damaged return address
                    0xC3});                       // ret
    Interpreter interpreter(t.machine);
    interpreter.addCode(kCode, kCode + 0x1000, "TestScript");
    callStopping(t, interpreter);
    CHECK(g_report == "[scripts] stopped: TestScript: a return or jump went to 0x00700000, outside its code; the "
                      "call into it fails, as does every later one");
    // The function's own search for its ret finds the one the stop holds, which pops nothing.
    CHECK(t.regs[kEsp] == kStackTop - 8);
    CHECK(t.regs[kEbx] == 0xB0B0B0B0u);
}

namespace {

// The script calls the host, which calls the script back at 0x1040, which stops.
void hostCallingBack();
Interpreter *g_backInterpreter = nullptr;
bool g_backReturned = false;
void hostCallingBack() {
    TestMachine &t = *g_current;
    t.regs[kEsp] -= 4;
    t.store32(t.regs[kEsp], kLiftedReturnAddress);
    g_backInterpreter->call(kCode + 0x40);
    g_backReturned = true;
    t.regs[kEsp] += 4; // the host's ret
}

} // namespace

TEST_CASE("a stop in a nested entry makes the entry waiting below it give up when the host returns") {
    TestMachine t;
    t.machine.report = &keepReport;
    t.machine.lookup = [](std::uint32_t address) -> HostFunction {
        return address == kHost ? &hostCallingBack : nullptr;
    };
    putStopping(t, {0xE8, 0xED, 0xEF, 0x4F, 0x00}); // call 0x00500000, the host
    t.put(kCode + 0x40, {0x0F, 0x0B});               // ud2
    Interpreter interpreter(t.machine);
    interpreter.addCode(kCode, kCode + 0x1000, "TestScript");
    g_backInterpreter = &interpreter;
    g_backReturned = false;
    callStopping(t, interpreter);
    CHECK(g_backReturned); // the host went on: only script frames gave up
    CHECK(g_report.find("it reached int3, hlt or ud2 at 0x00001040") != std::string::npos);
    CHECK(interpreter.stats().stops == 1u);
    CHECK(t.regs[kEax] == 0u); // not 9: the outer entry did not go on
    CHECK(t.regs[kEsp] == kStackTop);
    CHECK(t.regs[kEbx] == 0xB0B0B0B0u);
}

namespace {

constexpr std::uint32_t kHostDispatch = 0x00500010; // a host function that calls the script back

// Landing pads as the client keeps them: per guest thread, newest last, each resumed by a longjmp.
struct TestPad {
    std::jmp_buf jb;
    std::uint32_t entryEsp = 0;
    std::uint32_t function = 0;
    std::uint32_t resume = 0;
};
thread_local std::vector<TestPad *> g_pads;
Interpreter *g_interpreter = nullptr;
bool g_hostReturned = false;

void testRunInPad(std::uint32_t entryEsp, std::uint32_t function, void (*body)(void *, std::uint32_t),
                  void *context) {
    TestPad pad;
    pad.entryEsp = entryEsp;
    pad.function = function;
    g_pads.push_back(&pad);
    const bool resumed = setjmp(pad.jb) != 0;
    body(context, resumed ? pad.resume : 0);
    g_pads.pop_back();
}
void *testPadOwning(std::uint32_t esp, std::uint32_t *function) {
    for (std::size_t n = g_pads.size(); n > 0; --n) {
        if (g_pads[n - 1]->entryEsp > esp) {
            *function = g_pads[n - 1]->function;
            return g_pads[n - 1];
        }
    }
    return nullptr;
}
void testResumePad(void *pad, std::uint32_t pc) {
    auto *p = static_cast<TestPad *>(pad);
    while (g_pads.back() != p) {
        g_pads.pop_back();
    }
    p->resume = pc;
    std::longjmp(p->jb, 1);
}

// As the exception dispatcher calls a script's frame handler: a nested entry at 0x1040, after
// which it would go on (it does not, when the handler moves control to the catch).
void hostDispatch() {
    TestMachine &t = *g_current;
    t.regs[kEsp] -= 4;
    t.store32(t.regs[kEsp], kLiftedReturnAddress);
    g_interpreter->call(kCode + 0x40);
    g_hostReturned = true;
    t.regs[kEsp] += 4;
}

// The outer function saves its frame's esp and calls the host; the handler at 0x1040 switches the
// stack back to that frame and jumps to its continuation at 0x1020, which returns 7.
void putCatch(TestMachine &t) {
    t.put(kCode, {0x55,                               // push ebp
                  0x89, 0xE5,                         // mov ebp, esp
                  0x89, 0x25, 0x00, 0x30, 0x00, 0x00, // mov [0x3000], esp
                  0xE8, 0x02, 0xF0, 0x4F, 0x00,       // call 0x00500010
                  0xB8, 0x01, 0x00, 0x00, 0x00,       // mov eax, 1 (not reached)
                  0x5D,                               // pop ebp
                  0xC3});
    t.put(kCode + 0x20, {0xB8, 0x07, 0x00, 0x00, 0x00, // mov eax, 7
                         0x5D,                         // pop ebp
                         0xC3});
    t.put(kCode + 0x40, {0x8B, 0x25, 0x00, 0x30, 0x00, 0x00, // mov esp, [0x3000]
                         0x89, 0xE5,                         // mov ebp, esp
                         0xE9, 0xD3, 0xFF, 0xFF, 0xFF});     // jmp 0x1020
}

} // namespace

TEST_CASE("a catch in a nested entry goes on in the entry that owns its frame, over the host between") {
    TestMachine t;
    t.machine.lookup = [](std::uint32_t address) -> HostFunction {
        return address == kHostDispatch ? &hostDispatch : nullptr;
    };
    t.machine.runInPad = &testRunInPad;
    t.machine.padOwning = &testPadOwning;
    t.machine.resumePad = &testResumePad;
    putCatch(t);
    Interpreter interpreter(t.machine);
    interpreter.addCode(kCode, kCode + 0x1000);
    g_interpreter = &interpreter;
    g_hostReturned = false;
    t.run(interpreter);
    CHECK(t.regs[kEax] == 7u);
    CHECK_FALSE(g_hostReturned);
    CHECK(g_pads.empty());
    CHECK(interpreter.stats().entries == 2u);
    CHECK(interpreter.stats().resumes == 1u);
    // Again, from the cache: the pads and the depth came back as they were.
    t.regs[kEax] = 0;
    t.run(interpreter);
    CHECK(t.regs[kEax] == 7u);
    CHECK(g_pads.empty());
}

TEST_CASE("control that leaves upward with no landing pads stops the script with where it was going") {
    TestMachine t;
    t.machine.lookup = [](std::uint32_t address) -> HostFunction {
        return address == kHostDispatch ? &hostDispatch : nullptr;
    };
    t.machine.report = &keepReport;
    g_report.clear();
    putCatch(t);
    Interpreter interpreter(t.machine);
    interpreter.addCode(kCode, kCode + 0x1000);
    g_interpreter = &interpreter;
    t.regs[kEsp] = kStackTop - 4;
    t.store32(kStackTop - 4, kLiftedReturnAddress);
    interpreter.call(kCode);
    CHECK(g_report == "[scripts] stopped: a script: control left its frames upward, to a frame no interpreted "
                      "entry owns, going to 0x00001020; the call into it fails, as does every later one");
    CHECK(t.regs[kEax] == 0u);
}

namespace {

// Two guest threads handing the machine to each other, as the lifted game's yield does: one runs
// while the other waits, each with its own registers (kept here while it waits).
constexpr std::uint32_t kHandOver = 0x00500020; // thread A's host call: lets B run, then dispatches
constexpr std::uint32_t kWaitForA = 0x00500030; // thread B's host call: lets A run until A is done
constexpr std::uint32_t kStackB = 0x60000;

std::mutex g_turnLock;
std::condition_variable g_turnChanged;
char g_turn = 'A';
std::thread g_threadB;
std::uint32_t g_bEax = 0;

void giveTurn(char to) {
    TestMachine &t = *g_current;
    std::uint32_t mine[8];
    std::memcpy(mine, t.regs, sizeof mine);
    const char me = to == 'A' ? 'B' : 'A';
    std::unique_lock<std::mutex> lock(g_turnLock);
    g_turn = to;
    g_turnChanged.notify_all();
    g_turnChanged.wait(lock, [me] { return g_turn == me; });
    std::memcpy(t.regs, mine, sizeof mine);
}

void threadB() {
    {
        std::unique_lock<std::mutex> lock(g_turnLock);
        g_turnChanged.wait(lock, [] { return g_turn == 'B'; });
    }
    TestMachine &t = *g_current;
    t.regs[kEsp] = kStackB - 4;
    t.store32(kStackB - 4, kLiftedReturnAddress);
    g_interpreter->call(kCode + 0x100);
    g_bEax = t.regs[kEax];
    std::lock_guard<std::mutex> lock(g_turnLock);
    g_turn = 'A';
    g_turnChanged.notify_all();
}

// A's host call: B starts and runs until it waits in its own host call; then A's handler runs.
void handOver() {
    g_threadB = std::thread(threadB);
    giveTurn('B');
    hostDispatch();
}

void waitForA() {
    TestMachine &t = *g_current;
    t.regs[kEsp] += 4; // the host's ret
    giveTurn('A');
    t.regs[kEax] = 5;
}

} // namespace

TEST_CASE("a resume on one guest thread keeps the entries another thread is still running") {
    TestMachine t;
    t.machine.lookup = [](std::uint32_t address) -> HostFunction {
        if (address == kHandOver) {
            return &handOver;
        }
        if (address == kWaitForA) {
            return &waitForA;
        }
        return address == kHostDispatch ? &hostDispatch : nullptr;
    };
    t.machine.runInPad = &testRunInPad;
    t.machine.padOwning = &testPadOwning;
    t.machine.resumePad = &testResumePad;
    putCatch(t);
    t.put(kCode + 9, {0xE8, 0x12, 0xF0, 0x4F, 0x00}); // call 0x00500020 in place of the dispatch
    t.put(kCode + 0x100, {0xE8, 0x2B, 0xEF, 0x4F, 0x00, // call 0x00500030
                          0x83, 0xC0, 0x01,             // add eax, 1
                          0xC3});
    Interpreter interpreter(t.machine);
    interpreter.addCode(kCode, kCode + 0x100);          // A's script
    interpreter.addCode(kCode + 0x100, kCode + 0x200);  // B's
    g_interpreter = &interpreter;
    g_turn = 'A';
    t.run(interpreter); // A: entered, B entered and waiting, A's nested entry resumed A's first
    CHECK(t.regs[kEax] == 7u);
    CHECK(interpreter.stats().resumes == 1u);
    // B is still inside its script: its entry is running, so A's blocks removed now are kept
    // until it leaves (a link of B's could still reach them).
    CHECK(interpreter.activeEntries() == 1u);
    interpreter.removeCode(kCode, kCode + 0x100);
    CHECK(interpreter.retiredBlocks() != 0u);
    {
        std::unique_lock<std::mutex> lock(g_turnLock);
        g_turn = 'B';
        g_turnChanged.notify_all();
        g_turnChanged.wait(lock, [] { return g_turn == 'A'; });
    }
    g_threadB.join();
    CHECK(g_bEax == 6u);
    CHECK(interpreter.activeEntries() == 0u);
    CHECK(interpreter.retiredBlocks() == 0u);
}

namespace {

constexpr std::uint32_t kThrowingHost = 0x00500040; // a host function that unloads the script and throws
std::size_t g_entriesInHost = 0;
std::size_t g_retiredInHost = 0;

// A host function that fails with a C++ exception after its script's code was removed under it.
void throwingHost() {
    g_interpreter->removeCode(kCode, kCode + 0x1000);
    g_entriesInHost = g_interpreter->activeEntries();
    g_retiredInHost = g_interpreter->retiredBlocks();
    throw std::runtime_error("the host failed");
}

} // namespace

TEST_CASE("an exception through a call removes its entry, and the blocks retired under it are freed") {
    TestMachine t;
    t.machine.lookup = [](std::uint32_t address) -> HostFunction {
        return address == kThrowingHost ? &throwingHost : nullptr;
    };
    t.store32(0x2000, kThrowingHost);
    t.put(kCode, {0xFF, 0x15, 0x00, 0x20, 0x00, 0x00, // call [0x2000]
                  0xC3});
    Interpreter interpreter(t.machine);
    interpreter.addCode(kCode, kCode + 0x1000);
    g_interpreter = &interpreter;
    t.regs[kEsp] = kStackTop - 4;
    t.store32(kStackTop - 4, kLiftedReturnAddress);
    CHECK_THROWS_WITH_AS(interpreter.call(kCode), "the host failed", std::runtime_error);
    CHECK(g_entriesInHost == 1u);  // the call was running when its code went
    CHECK(g_retiredInHost != 0u);  // so its blocks were kept
    CHECK(interpreter.activeEntries() == 0u);
    CHECK(interpreter.retiredBlocks() == 0u);
}

TEST_CASE("rcl and rcr rotate through CF, one bit or several") {
    TestMachine t;
    t.run({0xF9,             // stc
           0xB0, 0x81,       // mov al, 0x81
           0xD0, 0xD0,       // rcl al, 1: 0x03, CF 1
           0x0F, 0x92, 0xC3, // setb bl
           0xF8,             // clc
           0xB2, 0x01,       // mov dl, 0x01
           0xD0, 0xDA,       // rcr dl, 1: 0x00, CF 1
           0x0F, 0x92, 0xC7, // setb bh
           0xF8,             // clc
           0xB1, 0x80,       // mov cl, 0x80
           0xC0, 0xD1, 0x02, // rcl cl, 2: 0x00 CF 1, then 0x01 CF 0
           0x0F, 0x92, 0xC5, // setb ch
           0xC3});
    CHECK(low(t.regs[kEax]) == 0x03);
    CHECK(low(t.regs[kEbx]) == 1);
    CHECK(low(t.regs[kEdx]) == 0x00);
    CHECK(high(t.regs[kEbx]) == 1);
    CHECK(low(t.regs[kEcx]) == 0x01);
    CHECK(high(t.regs[kEcx]) == 0);
}

TEST_CASE("shld and shrd shift in the other register's bits and leave the last bit out in CF") {
    TestMachine t;
    t.run({0xB8, 0x78, 0x56, 0x34, 0x12,             // mov eax, 0x12345678
           0xBA, 0xF0, 0xDE, 0xBC, 0x9A,             // mov edx, 0x9ABCDEF0
           0x0F, 0xA4, 0xD0, 0x08,                   // shld eax, edx, 8
           0x0F, 0x92, 0x05, 0x00, 0x30, 0x00, 0x00, // setb [0x3000]
           0xBB, 0x18, 0x00, 0x00, 0x00,             // mov ebx, 0x18
           0xBE, 0xFF, 0x00, 0x00, 0x00,             // mov esi, 0xFF
           0xB1, 0x04,                               // mov cl, 4
           0x0F, 0xAD, 0xF3,                         // shrd ebx, esi, cl
           0x0F, 0x92, 0x05, 0x01, 0x30, 0x00, 0x00, // setb [0x3001]
           0xC3});
    CHECK(t.regs[kEax] == 0x3456789Au);
    CHECK(t.memory[0x3000] == 0); // bit 24 of 0x12345678
    CHECK(t.regs[kEdx] == 0x9ABCDEF0u);
    CHECK(t.regs[kEbx] == 0xF0000001u);
    CHECK(t.memory[0x3001] == 1); // bit 3 of 0x18
    CHECK(t.regs[kEsi] == 0xFFu);
}

TEST_CASE("bt, bts, btr and btc: a register offset reaches the dwords around a memory operand") {
    TestMachine t;
    t.run({0xBB, 0x10, 0x30, 0x00, 0x00,             // mov ebx, 0x3010
           0xB8, 0x25, 0x00, 0x00, 0x00,             // mov eax, 37: the dword at +4, bit 5
           0x0F, 0xAB, 0x03,                         // bts [ebx], eax
           0xB8, 0xFF, 0xFF, 0xFF, 0xFF,             // mov eax, -1: the dword at -4, bit 31
           0x0F, 0xAB, 0x03,                         // bts [ebx], eax
           0xB8, 0x25, 0x00, 0x00, 0x00,             // mov eax, 37
           0x0F, 0xA3, 0x03,                         // bt [ebx], eax
           0x0F, 0x92, 0x05, 0x20, 0x30, 0x00, 0x00, // setb [0x3020]
           0x0F, 0xB3, 0x03,                         // btr [ebx], eax
           0x0F, 0x92, 0x05, 0x21, 0x30, 0x00, 0x00, // setb [0x3021]
           0x0F, 0xBB, 0x03,                         // btc [ebx], eax
           0x0F, 0x92, 0x05, 0x22, 0x30, 0x00, 0x00, // setb [0x3022]
           0x0F, 0xBA, 0x23, 0x25,                   // bt dword [ebx], 37: an immediate is masked to bit 5
           0x0F, 0x92, 0x05, 0x23, 0x30, 0x00, 0x00, // setb [0x3023]
           0x0F, 0xBA, 0x2B, 0x25,                   // bts dword [ebx], 37
           0xB9, 0x12, 0x00, 0x00, 0x00,             // mov ecx, 0x12
           0x0F, 0xBA, 0xF9, 0x21,                   // btc ecx, 33: bit 1
           0x0F, 0x92, 0x05, 0x24, 0x30, 0x00, 0x00, // setb [0x3024]
           0xC3});
    CHECK(t.load32(0x3014) == 0x20u);       // set, cleared, complemented back
    CHECK(t.load32(0x300C) == 0x80000000u); // below the operand
    CHECK(t.memory[0x3020] == 1);
    CHECK(t.memory[0x3021] == 1);
    CHECK(t.memory[0x3022] == 0);
    CHECK(t.memory[0x3023] == 0);
    CHECK(t.load32(0x3010) == 0x20u);
    CHECK(t.regs[kEcx] == 0x10u);
    CHECK(t.memory[0x3024] == 1);
}

namespace {

// Runs `code` at kCode as TestScript and returns the line it said. The test's guest memory is
// 1 MB: an access that reached the host past it would not come back.
std::string strayLine(std::initializer_list<std::uint8_t> code) {
    TestMachine t;
    t.machine.report = &keepReport;
    g_report.clear();
    t.put(kCode, code);
    Interpreter interpreter(t.machine);
    interpreter.addCode(kCode, kCode + 0x1000, "TestScript");
    t.regs[kEax] = 0x5555u;
    t.run(interpreter);
    CHECK(t.regs[kEax] == 0u); // the call failed
    CHECK(interpreter.stopped(kCode));
    return g_report;
}

} // namespace

TEST_CASE("an access past the guest's last byte stops the script and touches nothing outside the guest") {
    CHECK(strayLine({0xA1, 0xFE, 0xFF, 0xFF, 0xFF, // mov eax, [0xFFFFFFFE]
                     0xB8, 0x09, 0x00, 0x00, 0x00, // mov eax, 9
                     0xC3}) ==
          "[scripts] stopped: TestScript: it reached an access of 4 bytes at 0xFFFFFFFE, past the end of guest "
          "memory, in the block at 0x00001000 (bytes A1 FE FF FF); the call into it fails, as does every later one");
    CHECK(strayLine({0xBF, 0xF0, 0xFF, 0xFF, 0xFF, // mov edi, 0xFFFFFFF0
                     0xB9, 0x20, 0x00, 0x00, 0x00, // mov ecx, 0x20
                     0xB0, 0x55,                   // mov al, 0x55
                     0xF3, 0xAA,                   // rep stosb
                     0xC3})
              .find("an access of 32 bytes at 0xFFFFFFF0") != std::string::npos);
    CHECK(strayLine({0xBE, 0x00, 0x20, 0x00, 0x00, // mov esi, 0x2000
                     0xBF, 0xF8, 0xFF, 0xFF, 0xFF, // mov edi, 0xFFFFFFF8
                     0xB9, 0x04, 0x00, 0x00, 0x00, // mov ecx, 4
                     0xF3, 0xA5,                   // rep movsd
                     0xC3})
              .find("an access of 16 bytes at 0xFFFFFFF8") != std::string::npos);
    CHECK(strayLine({0xD9, 0xE8,                         // fld1
                     0xDB, 0x3D, 0xF8, 0xFF, 0xFF, 0xFF, // fstp tbyte [0xFFFFFFF8]
                     0xC3})
              .find("an access of 10 bytes at 0xFFFFFFF8") != std::string::npos);
    CHECK(strayLine({0xBB, 0xF0, 0xFF, 0xFF, 0xFF, // mov ebx, 0xFFFFFFF0
                     0x8B, 0x43, 0x0E,             // mov eax, [ebx+14]
                     0xC3})
              .find("an access of 4 bytes at 0xFFFFFFFE") != std::string::npos);
}

TEST_CASE("a call back to the script's own code counts as a back edge: deep recursion yields") {
    TestMachine t;
    t.yield = [] { ++g_current->yields; };
    t.run({0xB9, 0x01, 0x00, 0x01, 0x00, // mov ecx, 0x10001
           0xE8, 0x01, 0x00, 0x00, 0x00, // call 0x100B (forward)
           0xC3,                         // ret
           0x49,                         // 0x100B: dec ecx
           0x74, 0x05,                   // jz 0x1013
           0xE8, 0xF8, 0xFF, 0xFF, 0xFF, // call 0x100B (back): 0x10000 times
           0xC3});                       // 0x1013: ret
    CHECK(t.regs[kEcx] == 0u);
    CHECK(t.backEdges == 0x10000u);
    CHECK(t.yields == 1);
}

TEST_CASE("a long rep scan counts its iterations as back edges and yields as a loop does") {
    TestMachine t;
    t.yield = [] { ++g_current->yields; };
    t.run({0xBF, 0x00, 0x00, 0x09, 0x00, // mov edi, 0x90000
           0xB9, 0x00, 0x00, 0x02, 0x00, // mov ecx, 0x20000
           0xB0, 0x55,                   // mov al, 0x55: not in the zeroed memory
           0xF2, 0xAE,                   // repne scasb
           0xC3});
    CHECK(t.regs[kEcx] == 0u);
    CHECK(t.regs[kEdi] == 0xB0000u);
    CHECK(t.backEdges == 0x20000u);
    CHECK(t.yields == 2);
}
