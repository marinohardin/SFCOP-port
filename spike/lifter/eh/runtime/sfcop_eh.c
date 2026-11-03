/*
 * Guest structured and C++ exceptions for a lifted VC6 program on win32hle.
 *
 * The guest's static CRT is lifted with the rest of the program, so it already
 * holds everything a throw needs: _CxxThrowException, __CxxFrameHandler and its
 * catch matching, unwind funclets, the catch-object copy, _except_handler3 for
 * __try/__except. What it relies on from Windows is small, and this file is it:
 *
 *   RaiseException   walks the guest's own fs:[0] chain and calls each frame's
 *                    handler as guest code, exactly as the x86 dispatcher does
 *                    (EXCEPTION_RECORD and CONTEXT on the guest stack, a nested
 *                    record around each call so a throw from inside a handler
 *                    is flagged as nested);
 *   RtlUnwind        calls every handler above the target frame with
 *                    EXCEPTION_UNWINDING and unlinks it, then returns to the
 *                    caller (its TargetIp is always its own return address in
 *                    this CRT);
 *   faults           a host SIGSEGV/SIGBUS on guest memory, raised as the
 *                    guest's EXCEPTION_ACCESS_VIOLATION (sfcop_eh_guest_fault);
 *   IsBadReadPtr     and IsBadWritePtr/IsBadCodePtr, which the CRT's frame
 *                    handler uses to validate a ThrowInfo and its copy
 *                    constructor, answered from the host's mapping of the
 *                    guest block.
 *
 * The one thing lifted code cannot do itself is the last step. When a catch
 * has run, _JumpToContinuation loads the frame's esp and ebp and jumps to the
 * continuation; _except_handler3 calls an __except block the same way. On the
 * CPU that discards every frame below; lifted, each guest call is a host call,
 * so those frames are still on the host stack. The lift therefore turns those
 * two places into calls to sfcop_eh_jump_to_continuation() and
 * sfcop_eh_enter_except() (run_lift_eh.py), and every function that owns a
 * catching frame gets a setjmp landing pad at entry (RECOMP_EH_ENTER). The
 * transfer finds the newest pad whose entry esp is at or above the frame's
 * ebp, checks it against the continuation table the lift wrote, and longjmps
 * there; the pad's local dispatch goes to the continuation's label, or
 * tail-calls it when it lifted as an entry of its own.
 *
 * SFCOP_EH_TRACE=1 prints one line per raise, unwind and transfer.
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32hle.h"
#include "sfcop_eh.h"

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#else
#include <errno.h>
#include <unistd.h>
#endif

#define CHAIN_END             0xFFFFFFFFu
#define EXC_NONCONTINUABLE    0x01u
#define EXC_UNWINDING         0x02u
#define EXC_EXIT_UNWIND       0x04u
#define EXC_NESTED_CALL       0x10u
#define EXC_UNWIND_MASK       0x66u
#define STATUS_UNWIND         0xC0000027u
#define MSVC_CXX_EXCEPTION    0xE06D7363u
#define DISP_CONTINUE_EXEC    0u
#define DISP_CONTINUE_SEARCH  1u
#define DISP_NESTED           2u
#define DISP_COLLIDED         3u

/* x86 CONTEXT (0x2CC bytes) offsets used here. */
#define CTX_SIZE   0x2D0u
#define CTX_EDI    0x9Cu
#define CTX_ESI    0xA0u
#define CTX_EBX    0xA4u
#define CTX_EDX    0xA8u
#define CTX_ECX    0xACu
#define CTX_EAX    0xB0u
#define CTX_EBP    0xB4u
#define CTX_EIP    0xB8u
#define CTX_EFLAGS 0xC0u
#define CTX_ESP    0xC4u
#define ER_SIZE    0x50u

sfcop_eh_stats_t sfcop_eh_stats;
static int g_trace = -1;
static uint32_t g_nested_va;

/* --------------------------------------------------------- landing pads */
static __thread recomp_ehframe_t *t_frames;        /* newest first */
static __thread int t_in_fault;                     /* dispatching a hardware fault */

void recomp_eh_push(recomp_ehframe_t *f) { f->prev = t_frames; t_frames = f; }

void recomp_eh_pop(recomp_ehframe_t *f) {
    /* A longjmp past this frame has already dropped it. */
    for (recomp_ehframe_t *p = t_frames; p; p = p->prev)
        if (p == f) { t_frames = f->prev; return; }
}

recomp_ehframe_t *recomp_eh_owner(uint32_t esp) {
    for (recomp_ehframe_t *f = t_frames; f; f = f->prev)
        if (f->entry_esp > esp) return f;
    return NULL;
}

void recomp_eh_resume(recomp_ehframe_t *f, uint32_t resume) {
    t_frames = f;
    f->resume = resume;
    g_cur_func = f->func;
    longjmp(f->jb, 1);
}

static int tracing(void) {
    if (g_trace < 0) g_trace = getenv("SFCOP_EH_TRACE") != NULL;
    return g_trace;
}

/* Whether the lift's table lists `va` as a continuation (-1: no), and if so
 * whether `func` is one of its owners (1) or not (0). */
static int cont_owned_by(uint32_t va, uint32_t func) {
    uint32_t lo = 0, hi = sfcop_eh_cont_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (sfcop_eh_cont_table[mid][0] < va) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= sfcop_eh_cont_count || sfcop_eh_cont_table[lo][0] != va) return -1;
    for (; lo < sfcop_eh_cont_count && sfcop_eh_cont_table[lo][0] == va; lo++)
        if (sfcop_eh_cont_table[lo][1] == func) return 1;
    return 0;
}

/* Continue at `target` in the function whose frame g_ebp addresses, with the
 * guest registers already as the CPU would have them. Never returns. */
static void transfer(uint32_t target, const char *how) {
    recomp_ehframe_t *f = t_frames;
    while (f && f->entry_esp < g_ebp) f = f->prev;
    if (!f)
        hle_fatal("[eh] %s to 0x%08X (frame ebp 0x%08X): no landing pad above it", how, target, g_ebp);
    int owned = cont_owned_by(target, f->func);
    if (owned == 0)
        hle_fatal("[eh] %s to 0x%08X: the newest landing pad above ebp 0x%08X is sub_%08X's, which does not "
                  "own that continuation (its owner has no landing pad?)", how, target, g_ebp, f->func);
    if (owned < 0)
        fprintf(stderr, "[eh] %s to 0x%08X is not in the continuation table; resuming in sub_%08X\n", how,
                target, f->func);
    sfcop_eh_stats.transfers++;
    t_in_fault = 0;
    if (tracing())
        fprintf(stderr, "[eh] %s: resume at 0x%08X in sub_%08X (esp 0x%08X, ebp 0x%08X)\n", how, target,
                f->func, g_esp, g_ebp);
    recomp_eh_resume(f, target);
}

/* _JumpToContinuation(target, pRN), stdcall: unlink the top record (the
 * dispatcher's, relinked by _UnwindNestedFrames), take the frame's esp from
 * its slot below the record and its ebp from record + 12, jump. */
void sfcop_eh_jump_to_continuation(void) {
    uint32_t target = MEM32(g_esp + 4), rn = MEM32(g_esp + 8);
    uint32_t top = MEM32(g_fs_base);
    MEM32(g_fs_base) = MEM32(top);
    g_eax = target;
    g_ebx = rn;
    g_esp = MEM32(rn - 4);
    g_ebp = rn + 12;
    transfer(target, "catch");
}

/* _except_handler3 calls the __except block with ebp already the frame's; the
 * block begins `mov esp, [ebp-18h]`, so the call's own frame is discarded. */
void sfcop_eh_enter_except(uint32_t target) {
    g_esp -= 4;
    MEM32(g_esp) = RECOMP_RETADDR;
    transfer(target, "__except");
}

/* ------------------------------------------------------------ guest calls */
/* Call guest code at va with cdecl arguments, as a call from here would:
 * ebx, esi, edi, ebp and esp are the caller's again afterwards. */
static uint32_t guest_call(uint32_t va, int n, const uint32_t *args) {
    recomp_func_t fn = recomp_lookup_manual(va);
    if (!fn) fn = recomp_lookup(va);
    if (!fn) fn = recomp_lookup_import(va);
    if (!fn) hle_fatal("[eh] exception handler 0x%08X is not lifted", va);
    uint32_t esp = g_esp, ebp = g_ebp, ebx = g_ebx, esi = g_esi, edi = g_edi, cur = g_cur_func;
    for (int i = n - 1; i >= 0; i--) { g_esp -= 4; MEM32(g_esp) = args[i]; }
    g_esp -= 4;
    MEM32(g_esp) = RECOMP_RETADDR;
    fn();
    uint32_t eax = g_eax;
    g_esp = esp, g_ebp = ebp, g_ebx = ebx, g_esi = esi, g_edi = edi, g_cur_func = cur;
    return eax;
}

static void capture_context(uint32_t ctx, uint32_t eip, uint32_t esp) {
    memset(hle_ptr(ctx), 0, CTX_SIZE);
    MEM32(ctx) = 0x10007u;                            /* CONTEXT_FULL */
    MEM32(ctx + CTX_EDI) = g_edi, MEM32(ctx + CTX_ESI) = g_esi, MEM32(ctx + CTX_EBX) = g_ebx;
    MEM32(ctx + CTX_EDX) = g_edx, MEM32(ctx + CTX_ECX) = g_ecx, MEM32(ctx + CTX_EAX) = g_eax;
    MEM32(ctx + CTX_EBP) = g_ebp, MEM32(ctx + CTX_EIP) = eip, MEM32(ctx + CTX_ESP) = esp;
    MEM32(ctx + CTX_EFLAGS) = 0x202u;
}

/* The decorated name of the type a C++ exception record carries. */
static const char *cxx_type(uint32_t er) {
    if (MEM32(er) != MSVC_CXX_EXCEPTION || MEM32(er + 16) < 3) return NULL;
    uint32_t ti = MEM32(er + 28);
    if (!ti) return "(rethrow)";
    uint32_t cta = MEM32(ti + 12);
    if (!cta || !MEM32(cta)) return "?";
    return (const char *)hle_ptr(MEM32(MEM32(cta + 4) + 4) + 8);
}

/* The record the dispatcher puts on the chain around each handler call:
 * {next, handler, frame being handled}. A raise from inside that handler (a
 * throw in a catch block) meets it first; like Windows, it reports the nested
 * exception and names the frame, so the dispatcher flags the frames between. */
static void nested_frame_handler(void) {          /* cdecl (er, frame, ctx, dc) */
    uint32_t er = A32(0), frame = A32(1), dc = A32(3);
    if (MEM32(er + 4) & EXC_UNWIND_MASK) RET(DISP_CONTINUE_SEARCH, 0);
    MEM32(dc) = MEM32(frame + 8);
    RET(DISP_NESTED, 0);
}

/* Returns 1 if a handler asked to continue execution. */
static int dispatch(uint32_t er, uint32_t ctx, uint32_t scratch) {
    uint32_t nested_until = 0;
    uint32_t nest = scratch, dc = scratch + 12;
    for (uint32_t rec = MEM32(g_fs_base); rec && rec != CHAIN_END; rec = MEM32(rec)) {
        uint32_t handler = MEM32(rec + 4);
        MEM32(nest) = MEM32(g_fs_base);
        MEM32(nest + 4) = g_nested_va;
        MEM32(nest + 8) = rec;
        MEM32(g_fs_base) = nest;
        MEM32(dc) = 0;
        uint32_t a[4] = { er, rec, ctx, dc };
        uint32_t disp = guest_call(handler, 4, a);
        MEM32(g_fs_base) = MEM32(nest);
        if (nested_until == rec) {
            MEM32(er + 4) &= ~EXC_NESTED_CALL;
            nested_until = 0;
        }
        switch (disp) {
        case DISP_CONTINUE_EXEC:
            if (MEM32(er + 4) & EXC_NONCONTINUABLE)
                hle_fatal("[eh] handler 0x%08X continued a noncontinuable exception 0x%08X", handler, MEM32(er));
            return 1;
        case DISP_CONTINUE_SEARCH:
            break;
        case DISP_NESTED:
            MEM32(er + 4) |= EXC_NESTED_CALL;
            if (MEM32(dc) > nested_until) nested_until = MEM32(dc);
            break;
        default:
            hle_fatal("[eh] handler 0x%08X returned disposition %u", handler, disp);
        }
    }
    return 0;
}

/* For an unhandled exception: every frame on the chain and, for a C++ frame,
 * whether one of its try blocks covers its current state. */
static void report_chain(void) {
    int n = 0;
    for (uint32_t rec = MEM32(g_fs_base); rec && rec != CHAIN_END && n < 64; rec = MEM32(rec), n++) {
        uint32_t h = MEM32(rec + 4), fi = 0;
        if (sfcop_eh_guest_range_ok(h, 10, 0) && MEM8(h) == 0xB8 && MEM8(h + 5) == 0xE9) fi = MEM32(h + 1);
        if (fi && sfcop_eh_guest_range_ok(fi, 20, 0) && (MEM32(fi) & 0xFFFFFFF0u) == 0x19930520u) {
            int state = (int)MEM32(rec + 8), covered = 0;
            for (uint32_t t = 0; t < MEM32(fi + 12); t++) {
                uint32_t tb = MEM32(fi + 16) + 20 * t;
                if ((int)MEM32(tb) <= state && state <= (int)MEM32(tb + 4)) covered++;
            }
            fprintf(stderr, "[eh]   frame 0x%08X: C++, FuncInfo 0x%08X, state %d, %u try block(s), %d cover it\n",
                    rec, fi, state, MEM32(fi + 12), covered);
        } else {
            fprintf(stderr, "[eh]   frame 0x%08X: handler 0x%08X\n", rec, h);
        }
    }
    if (!n) fprintf(stderr, "[eh]   (the guest frame chain is empty)\n");
}

/* RaiseException(code, flags, nargs, args), stdcall. */
static void k_RaiseException(void) {
    uint32_t code = A32(0), flags = A32(1), n = A32(2), args = A32(3);
    uint32_t ret = MEM32(g_esp), esp0 = g_esp;
    if (n > 15) n = 15;
    g_esp = (g_esp - CTX_SIZE - ER_SIZE - 32) & ~15u;
    uint32_t ctx = g_esp, er = ctx + CTX_SIZE, scratch = er + ER_SIZE;
    memset(hle_ptr(er), 0, ER_SIZE);
    MEM32(er) = code;
    MEM32(er + 4) = flags & EXC_NONCONTINUABLE;
    MEM32(er + 12) = ret;
    MEM32(er + 16) = args ? n : 0;
    for (uint32_t i = 0; args && i < n; i++) MEM32(er + 20 + 4 * i) = MEM32(args + 4 * i);
    capture_context(ctx, ret, esp0 + 4 + 16);
    sfcop_eh_stats.raises++;
    const char *type = cxx_type(er);
    if (tracing())
        fprintf(stderr, "[eh] raise 0x%08X%s%s in sub_%08X\n", code, type ? " " : "", type ? type : "",
                g_cur_func);
    if (dispatch(er, ctx, scratch)) {
        g_edi = MEM32(ctx + CTX_EDI), g_esi = MEM32(ctx + CTX_ESI), g_ebx = MEM32(ctx + CTX_EBX);
        g_edx = MEM32(ctx + CTX_EDX), g_ecx = MEM32(ctx + CTX_ECX), g_ebp = MEM32(ctx + CTX_EBP);
        if (MEM32(ctx + CTX_EIP) != ret && ret != RECOMP_RETADDR)
            fprintf(stderr, "[eh] continuing at 0x%08X is not supported; returning to 0x%08X\n",
                    MEM32(ctx + CTX_EIP), ret);
        g_esp = esp0;
        RET(MEM32(ctx + CTX_EAX), 4);
    }
    sfcop_eh_stats.unhandled++;
    fprintf(stderr, "[eh] unhandled guest exception 0x%08X%s%s raised in sub_%08X\n", code,
            type ? ", C++ type " : "", type ? type : "", g_cur_func);
    report_chain();
    fflush(stderr);
    hle_exit(7);
}

/* A hardware fault in lifted code, as Windows delivers it: an
 * EXCEPTION_ACCESS_VIOLATION dispatched over the guest's chain. VC6's
 * catch(...) takes these too, and the game relies on it (its logger catches
 * a NULL string that way). Called from the host's signal handler on the
 * faulting thread; never returns if a frame takes it, returns 0 if none does.
 *
 * The faulting body keeps its registers in locals (RECOMP_LOCAL_REGS), so
 * g_esp may be as old as its entry, above what it has pushed since. The
 * records go 64 KB below it, clear of that; every frame they hand to a handler
 * is addressed from fs:[0] and ebp in guest memory, which is always current. */
int sfcop_eh_guest_fault(uint32_t code, uint32_t addr, int write) {
    if (t_in_fault) return 0;                        /* a fault while dispatching one */
    t_in_fault = 1;
    uint32_t esp0 = g_esp;
    g_esp = (g_esp - 0x10000u - CTX_SIZE - ER_SIZE - 32) & ~15u;
    uint32_t ctx = g_esp, er = ctx + CTX_SIZE, scratch = er + ER_SIZE;
    memset(hle_ptr(er), 0, ER_SIZE);
    MEM32(er) = code;
    MEM32(er + 12) = g_cur_func;
    MEM32(er + 16) = 2;
    MEM32(er + 20) = write ? 1 : 0;
    MEM32(er + 24) = addr;
    capture_context(ctx, g_cur_func, esp0);
    sfcop_eh_stats.raises++;
    sfcop_eh_stats.faults++;
    if (tracing())
        fprintf(stderr, "[eh] fault 0x%08X (%s) at guest 0x%08X in sub_%08X\n", code,
                write ? "write" : "read", addr, g_cur_func);
    if (dispatch(er, ctx, scratch))
        hle_fatal("[eh] a handler asked to continue after fault 0x%08X in sub_%08X; lifted code cannot "
                  "retry the access", code, g_cur_func);
    sfcop_eh_stats.unhandled++;
    fprintf(stderr, "[eh] unhandled guest fault 0x%08X (%s) at guest 0x%08X in sub_%08X\n", code,
            write ? "write" : "read", addr, g_cur_func);
    report_chain();
    g_esp = esp0;
    t_in_fault = 0;
    return 0;
}

/* RtlUnwind(TargetFrame, TargetIp, ExceptionRecord, ReturnValue), stdcall. */
static void k_RtlUnwind(void) {
    uint32_t target = A32(0), tip = A32(1), er = A32(2), rv = A32(3);
    uint32_t ret = MEM32(g_esp), esp0 = g_esp;
    g_esp = (g_esp - CTX_SIZE - ER_SIZE - 32) & ~15u;
    uint32_t ctx = g_esp, local = ctx + CTX_SIZE, dc = local + ER_SIZE;
    if (!er) {
        er = local;
        memset(hle_ptr(er), 0, ER_SIZE);
        MEM32(er) = STATUS_UNWIND;
        MEM32(er + 12) = ret;
    }
    MEM32(er + 4) |= EXC_UNWINDING | (target ? 0 : EXC_EXIT_UNWIND);
    capture_context(ctx, tip, esp0 + 4 + 16);
    MEM32(ctx + CTX_EAX) = rv;
    sfcop_eh_stats.unwinds++;
    uint32_t rec = MEM32(g_fs_base), n = 0;
    while (rec && rec != CHAIN_END && rec != target) {
        if (target && target < rec)
            hle_fatal("[eh] RtlUnwind target 0x%08X is below the chain head 0x%08X", target, rec);
        uint32_t a[4] = { er, rec, ctx, dc };
        MEM32(dc) = 0;
        uint32_t disp = guest_call(MEM32(rec + 4), 4, a);
        if (disp == DISP_COLLIDED) rec = MEM32(dc);
        else if (disp != DISP_CONTINUE_SEARCH)
            hle_fatal("[eh] unwind handler 0x%08X returned disposition %u", MEM32(rec + 4), disp);
        rec = MEM32(rec);
        MEM32(g_fs_base) = rec;
        n++;
    }
    if (target && rec != target)
        hle_fatal("[eh] RtlUnwind target 0x%08X is not on the guest's frame chain", target);
    if (tracing())
        fprintf(stderr, "[eh] unwind to 0x%08X: %u frame(s)\n", target, n);
    if (tip != ret && ret != RECOMP_RETADDR)      /* the lift pushes a dummy return address */
        fprintf(stderr, "[eh] RtlUnwind TargetIp 0x%08X is not its return address 0x%08X; returning\n", tip, ret);
    g_esp = esp0;
    RET(rv, 4);
}

/* ---------------------------------------------------------- bad pointers */
#ifdef __APPLE__
static int host_range_ok(uintptr_t p, uint64_t n, int want_write) {
    mach_vm_address_t a = p, end = p + n;
    while (a < end) {
        mach_vm_address_t r = a;
        mach_vm_size_t sz = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &r, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt,
                           &obj) != KERN_SUCCESS || r > a)
            return 0;
        if (!(info.protection & VM_PROT_READ) || (want_write && !(info.protection & VM_PROT_WRITE))) return 0;
        a = r + sz;
    }
    return 1;
}
#else
/* Portable read probe: the kernel copies from the range, or says EFAULT.
 * Writability is not probed (it would need a write); readable counts. */
static int host_range_ok(uintptr_t p, uint64_t n, int want_write) {
    (void)want_write;
    static int fds[2] = { -1, -1 };
    if (fds[0] < 0 && pipe(fds) != 0) return 1;
    char sink[4096];
    for (uint64_t off = 0; off < n; off += 4096) {
        size_t k = n - off < 4096 ? (size_t)(n - off) : 4096;
        if (write(fds[1], (const void *)(p + off), k) < 0) return errno != EFAULT;
        if (read(fds[0], sink, k) < 0) return 1;
    }
    return 1;
}
#endif

int sfcop_eh_guest_range_ok(uint32_t va, uint32_t n, int want_write) {
    if (!n) return 1;
    if (!va || (uint64_t)va + n > 0x100000000ull) return 0;
    return host_range_ok((uintptr_t)(void *)ADDR(va), n, want_write);
}

static void k_IsBadReadPtr(void)  { RET(!sfcop_eh_guest_range_ok(A32(0), A32(1), 0), 2); }
static void k_IsBadWritePtr(void) { RET(!sfcop_eh_guest_range_ok(A32(0), A32(1), 1), 2); }
static void k_IsBadCodePtr(void)  { RET(!sfcop_eh_guest_range_ok(A32(0), 1, 0), 1); }

static const win32hle_shim g_eh_shims[] = {
    { "RaiseException", k_RaiseException },
    { "RtlUnwind", k_RtlUnwind },
    { "IsBadReadPtr", k_IsBadReadPtr },
    { "IsBadWritePtr", k_IsBadWritePtr },
    { "IsBadCodePtr", k_IsBadCodePtr },
    { "sfcop_eh_nested_frame", nested_frame_handler },
    { 0, 0 }
};

static void (*g_prev_on_exit)(int);

static void print_stats(int code) {
    fprintf(stderr, "[eh] %u raise(s) (%u hardware fault(s)), %u transfer(s) into a catch or __except, "
            "%u unwind(s), %u unhandled\n", sfcop_eh_stats.raises, sfcop_eh_stats.faults,
            sfcop_eh_stats.transfers, sfcop_eh_stats.unwinds, sfcop_eh_stats.unhandled);
    if (g_prev_on_exit) g_prev_on_exit(code);
}

void sfcop_eh_install(void) {
    win32hle_register(g_eh_shims);
    g_nested_va = hle_resolve("sfcop_eh_nested_frame");
    if (!g_nested_va) hle_fatal("[eh] cannot register the dispatcher's frame handler");
    g_prev_on_exit = hle_on_exit;            /* hle_exit leaves with _exit: no atexit */
    hle_on_exit = print_stats;
}
