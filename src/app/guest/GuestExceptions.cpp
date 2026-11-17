#include "GuestExceptions.h"

#include "FaultClass.h"
#include "GuestHost.h"
#include "LowMemory.h"
#include "app/scripts/guest/GuestModuleAbi.h"
#include "fixes/fixes.h"

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
#include <setjmp.h> // _setjmp and _longjmp (POSIX), which RECOMP_EH_LONGJMP names
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
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

#if defined(__APPLE__)
// A fault handler on this thread that is still running, the newest first. The landing pads jump
// with _longjmp (RECOMP_EH_LONGJMP), which on macOS leaves the thread marked as running on its
// alternate signal stack when it jumps out of a handler; the next fault would then be handled on
// whatever stack the thread is using. So a jump to a pad pushed before the fault goes back into
// the handler first (back), and the handler returns into jumpToPad: the return ends the signal
// and clears the mark, as longjmp did. A pad pushed after the fault, by guest code the handler
// runs, is jumped to directly. Linux decides from the stack pointer, so it needs none of this.
//
// A record lives in its handler's frame and is taken off t_faultReturn on each of the handler's
// two exits: its return after a guest handler continued (the run then ends) and its own back
// jump. No other jump leaves the handler: one to a pad older than the fault goes through back,
// and one to a newer pad lands inside the handler, below its frame, so the record stays live and
// stays on t_faultReturn, as it must for the next jump to an older pad. A fault taken there is
// handled below that frame too, and its record names this one as outer.
struct FaultReturn {
    jmp_buf back;
    std::size_t pads; // pads [0, pads) are older than the fault
    recomp_ehframe_t *target;
    FaultReturn *outer;
};
thread_local FaultReturn *t_faultReturn = nullptr;
#endif

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

// Jumps to the landing pad at `index` of t_pads, through the fault handler it lies beyond, if any.
[[noreturn]] void jumpToPad(recomp_ehframe_t *frame, std::size_t index) {
#if defined(__APPLE__)
    if (FaultReturn *fault = t_faultReturn; fault != nullptr && index < fault->pads) {
        fault->target = frame;
        _longjmp(fault->back, 1);
    }
#endif
    static_cast<void>(index);
    RECOMP_EH_LONGJMP(frame->jb);
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

CodeRanges g_liftedCode; // set once by InstallExceptionHooks, read by the handler
std::uint32_t g_raiseShim = 0; // the RaiseException row, bound by BindRaiseException

// fix_hle_low_memory_reads: the armed region, or null (switch off); the game's image, for naming
// the code that read it. Set once by InstallLowMemoryReads, before the game runs.
LowMemoryRegion *g_lowMemory = nullptr;
std::uint32_t g_exeBase = 0, g_exeEnd = 0;

// The translated mission module whose library holds the host instruction at `pc`: its name (the
// library's stem) and its guest image, or false when `pc` is in no such library (or no longer
// loaded). Takes the loader's lock: never from a fault handler, only from ReportLowMemoryReads.
bool moduleOfPc(std::uintptr_t pc, char *stem, std::size_t size, const sfcop_guest_image **image) {
    *image = nullptr;
    stem[0] = '\0';
#if defined(_WIN32)
    HMODULE library = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(pc), &library) == 0) {
        return false;
    }
    char path[MAX_PATH];
    const DWORD length = GetModuleFileNameA(library, path, sizeof path);
    if (length == 0 || length >= sizeof path) {
        return false;
    }
    ModuleStem(path, stem, size);
    *image = reinterpret_cast<const sfcop_guest_image *>(GetProcAddress(library, SFCOP_GUEST_IMAGE_SYMBOL));
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<void *>(pc), &info) == 0 || info.dli_fname == nullptr) {
        return false;
    }
    ModuleStem(info.dli_fname, stem, size);
    // Already loaded (RTLD_NOLOAD): this only finds its handle, to look its image up.
    if (void *library = dlopen(info.dli_fname, RTLD_LAZY | RTLD_NOLOAD)) {
        *image = static_cast<const sfcop_guest_image *>(dlsym(library, SFCOP_GUEST_IMAGE_SYMBOL));
        dlclose(library);
    }
#endif
    return *image != nullptr;
}

// Who read low memory, for the report line: a function of the game's image by its address
// ("STARFLEETOP 0x00412345"), one of a translated mission by its offset in the module
// ("Met_17Patrol+0xEBB0"), else the current function and the host instruction. Outside the
// fault handler only (moduleOfPc).
void describeReader(std::uint32_t function, std::uintptr_t pc, char *out, std::size_t size) {
    if (function >= g_exeBase && function < g_exeEnd) {
        std::snprintf(out, size, "STARFLEETOP 0x%08X", function);
        return;
    }
    char stem[64];
    const sfcop_guest_image *image = nullptr;
    if (moduleOfPc(pc, stem, sizeof stem, &image) && function >= image->base &&
        function - image->base < image->size) {
        std::snprintf(out, size, "%s+0x%X", stem, function - image->base);
    } else if (stem[0] != '\0') {
        std::snprintf(out, size, "sub_%08X (in %s, pc %p)", function, stem, reinterpret_cast<void *>(pc));
    } else {
        std::snprintf(out, size, "sub_%08X (pc %p)", function, reinterpret_cast<void *>(pc));
    }
}

// A fault on guest `address` that the low-memory region takes. True when the faulting instruction
// is to be resumed. Signal-safe: the region maps the page and records the read (the current guest
// function and `pc`); ReportLowMemoryReads prints it later, outside the handler.
bool takeLowMemoryRead(std::uint32_t address, FaultAccess access, std::uintptr_t pc) {
    if (g_lowMemory == nullptr) {
        return false;
    }
    return g_lowMemory->OnFault(address, access, g_cur_func, pc) !=
           LowMemoryRegion::Outcome::kNotHandled;
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

// Where this fault handler runs, for its report line: on the thread's alternate signal stack, off
// it (each fault after a catch would be, on macOS, if a pad were reached by _longjmp straight out
// of the handler), or nothing for a thread with no alternate stack.
const char *alternateStackNote() {
    stack_t stack{};
    if (sigaltstack(nullptr, &stack) != 0 || (stack.ss_flags & SS_DISABLE) != 0) {
        return "";
    }
    const auto here = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
    const auto low = reinterpret_cast<std::uintptr_t>(stack.ss_sp);
    return here >= low && here - low < stack.ss_size ? ", on the alternate stack"
                                                     : ", OFF the alternate stack";
}

#if defined(__APPLE__)
// Where a fault handler's return lands when a pad older than the fault was chosen: the newest pad,
// which recomp_eh_resume has already made the last. The report line says which stack it ran on.
[[noreturn]] void resumeAfterFault() {
    std::fprintf(stderr, "[eh] resuming after a fault%s\n", alternateStackNote());
    jumpToPad(t_pads.back().frame, t_pads.size() - 1);
}

// Makes the interrupted thread, once the handler returns, call resumeAfterFault below this
// function's own frame: on the stack the handler ran on (the alternate stack, where the thread
// has one), whose frames are all finished by then. Not below the faulting stack pointer: a fault
// taken with the thread's stack nearly used up would leave no room there, and the jump would fault
// again in host code, which ends the run.
void returnIntoPad(void *context) {
    auto *ucontext = static_cast<ucontext_t *>(context);
    const auto entry = reinterpret_cast<std::uintptr_t>(&resumeAfterFault);
    const auto below = (reinterpret_cast<std::uint64_t>(__builtin_frame_address(0)) - 256) &
                       ~std::uint64_t{15};
#if defined(__aarch64__)
    auto &state = ucontext->uc_mcontext->__ss;
    __darwin_arm_thread_state64_set_sp(state, below);
    __darwin_arm_thread_state64_set_pc_fptr(state, reinterpret_cast<void *>(entry));
#elif defined(__x86_64__)
    auto &state = ucontext->uc_mcontext->__ss;
    state.__rsp = below - 8; // as after a call
    state.__rip = entry;
#else
#error "returnIntoPad: unknown macOS architecture"
#endif
}
#endif

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
    // A read of guest low memory that fix_hle_low_memory_reads answers, from any code: resume.
    // It comes before the machine-depth and t_inFault guards on purpose: it touches only the
    // region (mprotect, the fill, atomics) and resumes the instruction, so it is safe however
    // deeply faults nest, including a fault taken while another fault is being dispatched.
    if ((signal == SIGSEGV || signal == SIGBUS) && guestMemory &&
        takeLowMemoryRead(static_cast<std::uint32_t>(address - base), faultAccess(context), pc)) {
        return;
    }
    if ((signal == SIGSEGV || signal == SIGBUS) && guestMemory && hle_mach_depth() > 0 &&
        !t_inFault) {
        if (faultInLiftedCode(pc)) {
            // The guest's access violation first: a guest frame (catch(...), __except) may take
            // it, as on Windows. A handler that takes it longjmps out of this handler.
            t_inFault = true;
            const std::uint32_t parameters[2] = {AccessViolationFlag(faultAccess(context)),
                                                 static_cast<std::uint32_t>(address - base)};
            std::fprintf(stderr, "[eh] access violation: %s of guest 0x%08X in sub_%08X at %.3f s%s\n",
                         parameters[0] != 0 ? "write" : "read", parameters[1], g_cur_func,
                         Elapsed(), alternateStackNote());
#if defined(__APPLE__)
            FaultReturn back{};
            back.pads = t_pads.size();
            back.outer = t_faultReturn;
            t_faultReturn = &back;
            if (_setjmp(back.back) != 0) {
                t_faultReturn = t_faultReturn->outer;
                returnIntoPad(context);
                return; // into resumeAfterFault
            }
            RaiseFromHost(kAccessViolation, 2, parameters);
            t_faultReturn = back.outer;
#else
            RaiseFromHost(kAccessViolation, 2, parameters);
#endif
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
    // SA_NODEFER: a catch on Linux leaves the handler by a _longjmp, which does not unblock the
    // signal; the next fault must still be delivered.
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    for (int signal : {SIGSEGV, SIGBUS, SIGFPE, SIGILL}) {
        sigaction(signal, &action, nullptr);
    }
}

#else

// A Windows host reports a fault as an exception, first to the vectored handlers, on the faulting
// thread's own stack; the guest's handlers run there, inside the vectored handler, until one
// leaves by a longjmp, as they run inside the signal handler on a POSIX host. The main thread's
// stack is 8 MB for that (the sfcop target's /STACK).

std::uintptr_t faultPc(const CONTEXT *context) {
#if defined(_M_X64) || defined(__x86_64__)
    return static_cast<std::uintptr_t>(context->Rip);
#elif defined(_M_ARM64) || defined(__aarch64__)
    return static_cast<std::uintptr_t>(context->Pc);
#else
    static_cast<void>(context);
    return 0;
#endif
}

// The leaf memory routines' spans, found once, before any fault (InstallExceptionHooks).
CodeRanges g_memoryRoutines;

bool faultInLiftedCode(std::uintptr_t pc) {
    return pc == 0 || g_liftedCode.Contains(pc) || g_memoryRoutines.Contains(pc);
}

[[noreturn]] void endRun(const EXCEPTION_RECORD *record) {
    std::fprintf(stderr, "\n=== fault: exception 0x%08lX at %.3f s, in lifted sub_%08X, last import %s ===\n",
                 record->ExceptionCode, Elapsed(), g_cur_func,
                 g_cur_import != nullptr ? g_cur_import : "(none)");
    std::fflush(stderr);
    _exit(1);
}

// The signal handler's counterpart (onFault, on a POSIX host): a guest memory fault in lifted code
// is raised as the guest's access violation; a guest address touched by host code is a defect of
// the host and ends the run. Any other hardware fault is left to the frames that may handle it
// (Windows code may take its own with __try), and ends the run when none does (onUnhandled).
// Exceptions that are not faults (a C++ throw, a debugger's notice) are not touched.
LONG CALLBACK onException(EXCEPTION_POINTERS *pointers) {
    const EXCEPTION_RECORD *record = pointers->ExceptionRecord;
    const auto code = static_cast<std::uint32_t>(record->ExceptionCode);
    if (!IsWindowsHardwareFault(code)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (!IsWindowsMemoryFault(code) || record->NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto address = static_cast<std::uintptr_t>(record->ExceptionInformation[1]);
    const auto base = static_cast<std::uintptr_t>(g_mem_base); // RECOMP_MEM_BASE
    const bool guestMemory = address >= base && address - base < 0x100000000ull;
    if (!guestMemory) {
        return EXCEPTION_CONTINUE_SEARCH; // host memory: left to host frames and onUnhandled
    }
    // A read of guest low memory that fix_hle_low_memory_reads answers, from any code: resume.
    if (takeLowMemoryRead(static_cast<std::uint32_t>(address - base),
                          AccessOfWindowsFault(record->ExceptionInformation[0]),
                          faultPc(pointers->ContextRecord))) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    // Guest memory outside the machine lock, or faulting again while a fault is dispatched, ends
    // the run with the fault line alone, as the signal handler does.
    if (hle_mach_depth() <= 0 || t_inFault) {
        endRun(record);
    }
    const std::uintptr_t pc = faultPc(pointers->ContextRecord);
    if (faultInLiftedCode(pc)) {
        // The guest's access violation first: a guest frame (catch(...), __except) may take it,
        // as on Windows. A handler that takes it longjmps out of this handler.
        t_inFault = true;
        const std::uint32_t parameters[2] = {
            AccessViolationFlag(AccessOfWindowsFault(record->ExceptionInformation[0])),
            static_cast<std::uint32_t>(address - base)};
        std::fprintf(stderr, "[eh] access violation: %s of guest 0x%08X in sub_%08X at %.3f s\n",
                     parameters[0] != 0 ? "write" : "read", parameters[1], g_cur_func, Elapsed());
        RaiseFromHost(kAccessViolation, 2, parameters);
        std::fprintf(stderr, "[eh] a handler continued after an access violation at 0x%08X in "
                             "sub_%08X; lifted code cannot retry the access\n",
                     parameters[1], g_cur_func);
    } else {
        std::fprintf(stderr, "\n=== host fault: guest address 0x%08X touched by host code at "
                             "pc %p (a programming error; not a guest exception) ===\n",
                     static_cast<std::uint32_t>(address - base), reinterpret_cast<void *>(pc));
    }
    endRun(record);
}

LONG WINAPI onUnhandled(EXCEPTION_POINTERS *pointers) {
    if (IsWindowsHardwareFault(static_cast<std::uint32_t>(pointers->ExceptionRecord->ExceptionCode))) {
        endRun(pointers->ExceptionRecord);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

PVOID g_vectoredHandler = nullptr; // removed at exit

void removeFaultHandlers() {
    if (g_vectoredHandler != nullptr) {
        RemoveVectoredExceptionHandler(g_vectoredHandler);
        g_vectoredHandler = nullptr;
    }
}

// Installs once; a second call does nothing.
void installFaultHandlers() {
    static bool installed = false;
    if (installed) {
        return;
    }
    installed = true;
    for (const void *routine : {reinterpret_cast<const void *>(&std::memmove),
                                reinterpret_cast<const void *>(&std::memcpy),
                                reinterpret_cast<const void *>(&std::memset),
                                reinterpret_cast<const void *>(&std::memcmp)}) {
        const CodeRange span = MemoryRoutineSpan(routine);
        g_memoryRoutines.Add(span);
        if (win32hle_trace) {
            std::fprintf(stderr, "[eh] block-move routine %p: %zu bytes\n", routine,
                         static_cast<std::size_t>(span.end - span.begin));
        }
    }
    g_vectoredHandler = AddVectoredExceptionHandler(1, onException);
    SetUnhandledExceptionFilter(onUnhandled);
    std::atexit(removeFaultHandlers);
}

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

void InstallLowMemoryReads(std::uint32_t exeBase, std::uint32_t exeSize) {
    g_exeBase = exeBase;
    g_exeEnd = exeBase + exeSize;
#if defined(_WIN32)
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const std::size_t page = system.dwPageSize;
#else
    const auto page = static_cast<std::size_t>(getpagesize());
#endif
    static LowMemoryRegion region(reinterpret_cast<std::uint8_t *>(g_mem_base), page);
    // hle_vm_reserve maps the range committed, readable and writable on every host (on Windows
    // through VirtualAlloc with MEM_COMMIT), which is what Arm() protects.
    switch (SetUpLowMemory(fixes::isEnabled("fix_hle_low_memory_reads"), region, hle_vm_reserve,
                           hle_vm_release)) {
    case LowMemorySetUp::kOff:
        return; // the region stays unreserved: a read of it faults as without the fix
    case LowMemorySetUp::kMisaligned:
        std::fprintf(stderr, "[guest] fix_hle_low_memory_reads: guest 0x%08X is not at the start of a "
                             "host page of %zu bytes; off for this run\n",
                     kLowMemoryBegin, page);
        return;
    case LowMemorySetUp::kInUse:
        std::fprintf(stderr, "[guest] fix_hle_low_memory_reads: guest 0x%08X is in use; off for this run\n",
                     kLowMemoryBegin);
        return;
    case LowMemorySetUp::kCannotProtect:
        std::fprintf(stderr, "[guest] fix_hle_low_memory_reads: cannot protect guest 0x%08X (host "
                             "page %zu bytes); off for this run\n",
                     kLowMemoryBegin, page);
        return;
    case LowMemorySetUp::kArmed:
        break;
    }
    g_lowMemory = &region;
    std::fprintf(stderr, "[guest] fix_hle_low_memory_reads: guest 0x%08X-0x%08X reserved, %zu pages "
                         "of %zu bytes, each readable from its first read\n",
                 kLowMemoryBegin, kLowMemoryEnd - 1, region.PageCount(), page);
}

void ReportLowMemoryReads() {
    if (g_lowMemory == nullptr) {
        return;
    }
    LowMemoryRegion::Read reads[4];
    for (std::size_t count; (count = g_lowMemory->TakeReads(reads, 4)) != 0;) {
        for (std::size_t i = 0; i < count; ++i) {
            char reader[160];
            describeReader(reads[i].function, reads[i].pc, reader, sizeof reader);
            char line[256];
            const std::size_t length = FormatLowMemoryRead(line, sizeof line, reader, reads[i].address);
            std::fwrite(line, 1, length, stderr);
        }
        std::fflush(stderr);
    }
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
    sfcop::guest::jumpToPad(frame, i);
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
