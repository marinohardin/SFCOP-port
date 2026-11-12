// The guest machine an interpreted script runs on: the registers, flags, x87 stack and memory that
// the lifted game's code keeps as globals of the running client (pcrecomp's register model, the
// same ones a translated mission module binds by name), and the lifter's runtime calls the
// interpreter needs. The client fills this from its own symbols (InterpretedModules.cpp); a test
// fills it with storage of its own.
//
// Between functions the lifted code keeps its arithmetic flags as a lazy triple (a kind, two
// operands) plus a carry; every call hands the caller's over and every return the callee's. The
// interpreter keeps the same triple in the same encoding, so flags cross in both directions as
// they do between lifted functions.
//
// Threading: used with the guest machine held, so by one thread at a time.
#pragma once

#include <cstdint>

namespace sfcop::scripts::interp {

using HostFunction = void (*)(void);

struct Machine {
    // eax, ecx, edx, ebx, esp, ebp, esi, edi: the order of the x86 register numbers.
    std::uint32_t *regs[8] = {};
    std::uint32_t *flagKind = nullptr; // the lifter's g_flag_k, g_flag_a, g_flag_b, g_flag_cf
    std::uint32_t *flagA = nullptr;
    std::uint32_t *flagB = nullptr;
    std::uint32_t *flagCarry = nullptr;
    double *st = nullptr;         // g_st[8]: st(0) first; a push shifts the array down
    std::int64_t *stExact = nullptr; // g_st_i64[8]: the exact value fild qword loaded
    int *fpTop = nullptr;         // g_fp_top: the count of pushes less pops
    std::uint16_t *fpuControl = nullptr; // g_fpu_cw
    std::uint32_t *fsBase = nullptr; // the thread's TIB, the base of fs:
    std::uint32_t *gsBase = nullptr;
    std::uint32_t *currentFunction = nullptr; // g_cur_func, for the runtime's fault reports
    std::uint8_t *memory = nullptr;           // guest address 0, on the host

    // The function that answers a guest address outside every interpreted image, as an indirect
    // call in lifted code asks for it (manual hooks, the lifted dispatch, then the import rows),
    // or null when nothing answers it.
    HostFunction (*lookup)(std::uint32_t address) = nullptr;
    // A loop's back edge: every 0x10000 of them the lifted code hands the machine to other guest
    // threads through *yieldHook, when there is one.
    void (**yieldHook)(void) = nullptr;
    std::uint32_t *backEdges = nullptr;
    // A zero divisor: the hook raises the guest's EXCEPTION_INT_DIVIDE_BY_ZERO, when there is one.
    void (**divideFaultHook)(std::uint32_t code) = nullptr;
    // Landing pads (the lift's recomp_eh_push, recomp_eh_owner, recomp_eh_resume). Every entry into
    // script code runs inside one, as a lifted function with a catching frame does: runInPad pushes
    // a pad for an entry whose esp is entryEsp, calls body(context, 0), and each time the pad is
    // resumed calls body(context, pc) again, the guest registers already set; it pops the pad when
    // body returns. A catch, an __except block or a longjmp in a script's own C runtime moves the
    // guest stack above the entry that runs it (the exception dispatcher's call into the script's
    // handler): the interpreter then asks padOwning for the pad whose entry owns that stack address
    // and the function it was entered for, and resumePad moves control there, over the host frames
    // in between, as the lift's own transfers do. Null in a machine without guest exceptions.
    void (*runInPad)(std::uint32_t entryEsp, std::uint32_t function, void (*body)(void *context, std::uint32_t pc),
                     void *context) = nullptr;
    void *(*padOwning)(std::uint32_t esp, std::uint32_t *function) = nullptr;
    void (*resumePad)(void *pad, std::uint32_t pc) = nullptr; // does not return
    // Control the interpreter cannot follow (to an address outside every script's code, or up to
    // a frame no entry owns): the sentence says what and where. Null: written to stderr, then
    // abort. It does not return.
    void (*fatal)(const char *message) = nullptr;
    // A line for the player and the mod author (a script stopped). Null: written to stderr.
    void (*report)(const char *line) = nullptr;
};

// The return address lifted code pushes for a call (RECOMP_RETADDR); its `ret` pops it unread.
constexpr std::uint32_t kLiftedReturnAddress = 0xDEAD0000u;
// The back edges between two hand-overs (RECOMP_YIELD_EVERY).
constexpr std::uint32_t kYieldEvery = 0x10000u;
// EXCEPTION_INT_DIVIDE_BY_ZERO.
constexpr std::uint32_t kDivideByZero = 0xC0000094u;
// EXCEPTION_INT_OVERFLOW: a divide whose quotient does not fit raises #DE, which Windows reports as this.
constexpr std::uint32_t kIntegerOverflow = 0xC0000095u;

} // namespace sfcop::scripts::interp
