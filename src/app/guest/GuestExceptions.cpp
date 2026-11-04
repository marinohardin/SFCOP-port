#include "GuestExceptions.h"

#include "FaultClass.h"
#include "GuestHost.h"

extern "C" {
#include "win32hle.h"
// The lift's table of every continuation a catch can resume at, with the function that owns it
// (cmake/lifter/run_lift.py), sorted by continuation.
extern const uint32_t sfcop_eh_cont_table[][2];
extern const uint32_t sfcop_eh_cont_count;
// The lift's dispatch table (recomp_types.h): the address of every lifted function.
extern const recomp_dispatch_entry_t recomp_dispatch_table[];
extern const uint32_t recomp_dispatch_count;
}

#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <vector>

#if !defined(_WIN32)
#include <dlfcn.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif
#endif

namespace sfcop::guest {

namespace {

// This thread's landing pads, oldest first, each with the machine lock's depth at its entry.
struct Pad {
    recomp_ehframe_t *frame;
    int depth;
};
thread_local std::vector<Pad> t_pads;
thread_local bool t_inFault = false; // dispatching a hardware fault on this thread

constexpr std::uint32_t kAccessViolation = 0xC0000005u;

std::size_t padIndex(const recomp_ehframe_t *frame) {
    for (std::size_t i = t_pads.size(); i > 0; --i) {
        if (t_pads[i - 1].frame == frame) {
            return i - 1;
        }
    }
    return t_pads.size();
}

// Whether the lift lists `target` as a continuation (-1: no), and if so whether `owner` is one
// of the functions it belongs to (1) or not (0).
int continuationOwnedBy(std::uint32_t target, std::uint32_t owner) {
    std::uint32_t low = 0, high = sfcop_eh_cont_count;
    while (low < high) {
        const std::uint32_t middle = (low + high) / 2;
        if (sfcop_eh_cont_table[middle][0] < target) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low >= sfcop_eh_cont_count || sfcop_eh_cont_table[low][0] != target) {
        return -1;
    }
    for (; low < sfcop_eh_cont_count && sfcop_eh_cont_table[low][0] == target; ++low) {
        if (sfcop_eh_cont_table[low][1] == owner) {
            return 1;
        }
    }
    return 0;
}

// Continues at `target` in the function whose frame ebp addresses, the guest registers already as
// the CPU would have them: the newest pad whose entry is at or above that frame.
[[noreturn]] void transfer(std::uint32_t target, const char *how) {
    std::size_t i = t_pads.size();
    while (i > 0 && t_pads[i - 1].frame->entry_esp < g_ebp) {
        --i;
    }
    if (i == 0) {
        hle_fatal("[eh] %s to 0x%08X (frame ebp 0x%08X): no landing pad above it", how, target, g_ebp);
    }
    recomp_ehframe_t *frame = t_pads[i - 1].frame;
    const int owned = continuationOwnedBy(target, frame->func);
    if (owned == 0) {
        hle_fatal("[eh] %s to 0x%08X: the newest landing pad above ebp 0x%08X is sub_%08X's, which "
                  "does not own that continuation",
                  how, target, g_ebp, frame->func);
    }
    if (owned < 0) {
        std::fprintf(stderr, "[eh] %s to 0x%08X is not in the continuation table; resuming in "
                             "sub_%08X\n",
                     how, target, frame->func);
    }
    t_inFault = false;
    if (win32hle_trace) {
        std::fprintf(stderr, "[eh] %s: resume at 0x%08X in sub_%08X (esp 0x%08X, ebp 0x%08X)\n", how,
                     target, frame->func, g_esp, g_ebp);
    }
    recomp_eh_resume(frame, target);
    std::abort(); // recomp_eh_resume does not return
}

void onDivideFault(std::uint32_t code) {
    RaiseFromHost(code, 0, nullptr); // a handler that continues gets the lift's result, 0
}

#if !defined(_WIN32)

// What the fault context says, where this platform's layout is known; else "unknown" (the
// dispatcher records a read) and pc 0 (the fault is taken to be in lifted code).
FaultAccess faultAccess(void *context) {
    const auto *ucontext = static_cast<ucontext_t *>(context);
#if defined(__APPLE__) && defined(__aarch64__)
    // The data abort's WnR bit (ESR bit 6).
    return ((ucontext->uc_mcontext->__es.__esr >> 6) & 1u) != 0 ? FaultAccess::kWrite
                                                                : FaultAccess::kRead;
#elif defined(__APPLE__) && defined(__x86_64__)
    return (ucontext->uc_mcontext->__es.__err & 2u) != 0 ? FaultAccess::kWrite : FaultAccess::kRead;
#elif defined(__linux__) && defined(__x86_64__)
    // The page-fault error code: bit 1 is set for a write.
    return (ucontext->uc_mcontext.gregs[REG_ERR] & 2) != 0 ? FaultAccess::kWrite : FaultAccess::kRead;
#else
    static_cast<void>(ucontext);
    return FaultAccess::kUnknown; // other platforms: not read from the context
#endif
}

std::uintptr_t faultPc(void *context) {
    const auto *ucontext = static_cast<ucontext_t *>(context);
#if defined(__APPLE__) && defined(__aarch64__)
    return static_cast<std::uintptr_t>(__darwin_arm_thread_state64_get_pc(ucontext->uc_mcontext->__ss));
#elif defined(__APPLE__) && defined(__x86_64__)
    return static_cast<std::uintptr_t>(ucontext->uc_mcontext->__ss.__rip);
#elif defined(__linux__) && defined(__x86_64__)
    return static_cast<std::uintptr_t>(ucontext->uc_mcontext.gregs[REG_RIP]);
#elif defined(__linux__) && defined(__aarch64__)
    return static_cast<std::uintptr_t>(ucontext->uc_mcontext.pc);
#else
    static_cast<void>(ucontext);
    return 0;
#endif
}

CodeRanges g_liftedCode; // set once by InstallExceptionHooks, read by the handler
std::uint32_t g_raiseShim = 0; // the RaiseException row, bound by BindRaiseException

// A fault is the guest's own when the instruction that faulted is lifted code, or a leaf memory
// routine that lifted code calls for a block move (no host frame lies between it and its lifted
// caller). A pc the platform cannot read counts as lifted.
bool faultInLiftedCode(std::uintptr_t pc) {
    if (pc == 0 || g_liftedCode.Contains(pc)) {
        return true;
    }
    Dl_info info{};
    return dladdr(reinterpret_cast<void *>(pc), &info) != 0 && IsLeafMemoryRoutine(info.dli_sname);
}

// Guest memory faults become guest exceptions only when raised from lifted code. The exception
// is delivered by a longjmp over every frame between the fault and the catching function; frames
// of host code among them would be skipped without running their destructors, leaving locks held
// and strings leaked, which is undefined behaviour. A fault in host code (a module reading a
// guest pointer it should have checked) is a defect of the host and ends the run, naming the
// instruction and the address. Known limit: a module that touches guest memory the guest handed
// it unchecked is not given the guest's handlers, as Windows' own API would be (it returns an
// error instead); such a module checks with guestRangeReadable first.
void onFault(int signal, siginfo_t *info, void *context) {
    const auto address = reinterpret_cast<std::uintptr_t>(info->si_addr);
    const auto base = static_cast<std::uintptr_t>(g_mem_base); // RECOMP_MEM_BASE
    const bool guestMemory = address >= base && address - base < 0x100000000ull;
    const std::uintptr_t pc = faultPc(context);
    if ((signal == SIGSEGV || signal == SIGBUS) && guestMemory && hle_mach_depth() > 0 &&
        !t_inFault) {
        if (faultInLiftedCode(pc)) {
            // The guest's access violation first: a guest frame (catch(...), __except) may take
            // it, as on Windows. A handler that takes it longjmps out of this handler.
            t_inFault = true;
            const std::uint32_t parameters[2] = {AccessViolationFlag(faultAccess(context)),
                                                 static_cast<std::uint32_t>(address - base)};
            RaiseFromHost(kAccessViolation, 2, parameters);
            std::fprintf(stderr, "[eh] a handler continued after an access violation at 0x%08X in "
                                 "sub_%08X; lifted code cannot retry the access\n",
                         parameters[1], g_cur_func);
        } else {
            std::fprintf(stderr, "\n=== host fault: guest address 0x%08X touched by host code at "
                                 "pc %p (a programming error; not a guest exception) ===\n",
                         static_cast<std::uint32_t>(address - base),
                         reinterpret_cast<void *>(pc));
        }
    }
    std::fprintf(stderr, "\n=== fault: signal %d at %.3f s, in lifted sub_%08X, last import %s ===\n",
                 signal, Elapsed(), g_cur_func, g_cur_import != nullptr ? g_cur_import : "(none)");
    std::fflush(stderr);
    _exit(1);
}

void installFaultHandlers() {
    // The main thread's handlers run on a stack of their own, large enough for the guest's
    // handlers and catch blocks, which run inside the signal handler until one jumps out.
    static std::vector<std::uint8_t> alternate(8u << 20);
    stack_t stack{};
    stack.ss_sp = alternate.data();
    stack.ss_size = alternate.size();
    sigaltstack(&stack, nullptr);
    struct sigaction action {};
    action.sa_sigaction = onFault;
    // SA_NODEFER: a catch leaves the handler by a longjmp, which does not unblock the signal
    // everywhere; the next fault must still be delivered.
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    for (int signal : {SIGSEGV, SIGBUS, SIGFPE, SIGILL}) {
        sigaction(signal, &action, nullptr);
    }
}

#else

// pcrecomp's win32hle host is POSIX-only; the lift does not build on Windows yet.
void installFaultHandlers() {}

#endif

} // namespace

void InstallExceptionHooks() {
    // The lifted functions' span, with 32 KB for the last one's body.
    std::vector<std::uintptr_t> functions;
    functions.reserve(recomp_dispatch_count);
    for (std::uint32_t i = 0; i < recomp_dispatch_count; ++i) {
        functions.push_back(reinterpret_cast<std::uintptr_t>(recomp_dispatch_table[i].func));
    }
    g_liftedCode.Add(SpanOfFunctions(functions.data(), functions.size(), 32u << 10));
    recomp_div_fault_hook = onDivideFault;
    installFaultHandlers();
}

void BindRaiseException() {
    g_raiseShim = hle_resolve("RaiseException"); // looked up here, never inside a signal handler
}

void RaiseFromHost(std::uint32_t code, std::uint32_t count, const std::uint32_t *parameters) {
    // A faulting body keeps its registers in locals, so esp may be as old as its entry: the call
    // goes 64 KB below it, clear of whatever the body has pushed since.
    const std::uint32_t entryEsp = g_esp;
    g_esp = (g_esp - 0x10000u) & ~15u;
    g_esp -= 64;
    const std::uint32_t arguments = g_esp;
    for (std::uint32_t i = 0; i < count && i < 15; ++i) {
        MEM32(arguments + 4 * i) = parameters[i];
    }
    const std::uint32_t pushed[4] = {code, 0, count, count != 0 ? arguments : 0};
    for (std::uint32_t i = 4; i > 0; --i) {
        g_esp -= 4;
        MEM32(g_esp) = pushed[i - 1];
    }
    g_esp -= 4;
    MEM32(g_esp) = RECOMP_RETADDR;
    // pcrecomp hands every import the same entry and passes which import it is through a global
    // the lookup sets, so the lookup comes right before the call, never cached.
    const recomp_func_t raise = recomp_lookup_import(g_raiseShim);
    if (raise == nullptr) {
        hle_fatal("[eh] no RaiseException row to raise 0x%08X with", code);
    }
    raise();
    g_esp = entryEsp;
}

} // namespace sfcop::guest

// ---- the landing pads and the two transfers the lift calls (cmake/lifter/run_lift.py) ----

using sfcop::guest::Pad;
using sfcop::guest::t_pads;

extern "C" void recomp_eh_push(recomp_ehframe_t *frame) {
    frame->prev = t_pads.empty() ? nullptr : t_pads.back().frame;
    t_pads.push_back(Pad{frame, hle_mach_depth()});
}

// A longjmp past this pad has already dropped it.
extern "C" void recomp_eh_pop(recomp_ehframe_t *frame) {
    const std::size_t i = sfcop::guest::padIndex(frame);
    if (i < t_pads.size()) {
        t_pads.resize(i);
    }
}

extern "C" recomp_ehframe_t *recomp_eh_owner(uint32_t esp) {
    for (std::size_t i = t_pads.size(); i > 0; --i) {
        if (t_pads[i - 1].frame->entry_esp > esp) {
            return t_pads[i - 1].frame;
        }
    }
    return nullptr;
}

extern "C" void recomp_eh_resume(recomp_ehframe_t *frame, uint32_t resume) {
    const std::size_t i = sfcop::guest::padIndex(frame);
    if (i == t_pads.size()) {
        hle_fatal("[eh] resume at 0x%08X: its landing pad is not on this thread", resume);
    }
    // Every native-to-guest call jumped over skipped its mach_leave; the thread holds the machine
    // at both depths, so only the count goes back.
    hle_mach_set_depth(t_pads[i].depth);
    t_pads.resize(i + 1);
    frame->resume = resume;
    g_cur_func = frame->func;
    std::longjmp(frame->jb, 1);
}

// _JumpToContinuation(target, registrationNode), stdcall: unlink the top record (the dispatcher's,
// relinked by _UnwindNestedFrames), take the frame's esp from the slot below the record and its ebp
// from record + 12, and jump.
extern "C" void sfcop_eh_jump_to_continuation(void) {
    const std::uint32_t target = MEM32(g_esp + 4), node = MEM32(g_esp + 8);
    const std::uint32_t top = MEM32(g_fs_base);
    MEM32(g_fs_base) = MEM32(top);
    g_eax = target;
    g_ebx = node;
    g_esp = MEM32(node - 4);
    g_ebp = node + 12;
    sfcop::guest::transfer(target, "catch");
}

// _except_handler3 calls the __except block with ebp already the frame's; the block begins
// `mov esp, [ebp-18h]`, so the call's own frame is discarded.
extern "C" void sfcop_eh_enter_except(uint32_t target) {
    g_esp -= 4;
    MEM32(g_esp) = RECOMP_RETADDR;
    sfcop::guest::transfer(target, "__except");
}
