// Guest-side exception tests for the guest host (src/app/guest/README.md, "Tests").
//
// Built with the VC6 SP4 compiler in toolchain/ (/GX, static CRT), lifted with the same driver
// as the game and run by the guest test host. Each case prints one PASS or FAIL line; the exit
// code is the number of failures. Expected values are written out.
// `ehtest uncaught` throws an int nothing catches: as on Windows, the C runtime's top-level filter
// calls terminate(), which prints "abnormal program termination" and exits with status 3 (the
// test host's exit status 1).
#include <stdio.h>
#include <string.h>
#include <windows.h>

static int g_failures;
static char g_log[256];     // destructor order, one letter per destructor

static void check(const char *name, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) g_failures++;
}

static void log_char(char c) {
    size_t n = strlen(g_log);
    if (n + 1 < sizeof g_log) { g_log[n] = c; g_log[n + 1] = 0; }
}

struct Guard {
    char tag;
    explicit Guard(char t) : tag(t) {}
    ~Guard() { log_char(tag); }
};

struct Error {
    int code;
    static int live;
    explicit Error(int c) : code(c) { live++; }
    Error(const Error &o) : code(o.code) { live++; }
    virtual ~Error() { live--; }
};
int Error::live;

struct DerivedError : Error {
    explicit DerivedError(int c) : Error(c) {}
};

// /O2 in VC6 inlines only what is marked inline (/Ob1), so these stay out of line
// and each throw really leaves its frame.
static void throw_int(int v) { if (v) throw v; }
static void throw_error(int v) { if (v) throw Error(v); }
static void throw_derived(int v) { if (v) throw DerivedError(v); }

static int case_int(void) {
    try {
        throw_int(42);
    } catch (int v) {
        return v;
    }
    return -1;
}

static int case_class_by_value(void) {
    try {
        throw_error(7);
    } catch (Error e) {
        return e.code;
    }
    return -1;
}

static int case_class_by_ref(void) {
    try {
        throw_derived(9);
    } catch (Error &e) {
        return e.code;
    }
    return -1;
}

static int case_nested(void) {
    int r = 0;
    try {
        try {
            throw_int(5);
        } catch (float) {
            r = -100;               // wrong type: must not run
        }
        r = -200;                   // the inner try must not resume here
    } catch (int v) {
        r += v;
    }
    return r;
}

static int case_rethrow(void) {
    int r = 0;
    try {
        try {
            throw_error(3);
        } catch (...) {
            r = 10;
            throw;
        }
    } catch (Error &e) {
        r += e.code;
    }
    return r;
}

static int case_throw_from_catch(void) {
    int r = 0;
    try {
        try {
            throw_int(1);
        } catch (int) {
            throw_error(20);
        }
    } catch (Error &e) {
        r = e.code;
    }
    return r;
}

static void inner_frame(int v) {
    Guard b('B');
    throw_int(v);
    log_char('x');                  // not reached
}

static void outer_frame(int v) {
    Guard a('A');
    inner_frame(v);
    log_char('y');                  // not reached
}

static int case_unwind_two_frames(void) {
    g_log[0] = 0;
    try {
        Guard c('C');
        outer_frame(11);
    } catch (int v) {
        log_char('0' + (char)(v - 10));
    }
    return strcmp(g_log, "BAC1") == 0;
}

static int case_loop(void) {
    int caught = 0;
    for (int i = 1; i <= 100; i++) {
        try {
            Guard g('L');
            throw_int(i);
        } catch (int v) {
            caught += v;
        }
    }
    return caught;
}

static int case_seh(void) {
    int r = 0;
    __try {
        RaiseException(0xE0001234u, 0, 0, NULL);
        r = -1;
    } __except (GetExceptionCode() == 0xE0001234u ? EXCEPTION_EXECUTE_HANDLER
                                                  : EXCEPTION_CONTINUE_SEARCH) {
        r = 77;
    }
    return r;
}

// An access violation is a structured exception: VC6's catch(...) takes it, and
// so does __except with the right filter. The game's logger relies on the first.
// The faulting store sits in a called function, as in the game (strlen(NULL)):
// /GX lets the compiler drop a try block around code that makes no calls.
static volatile int *g_null;
static void poke(volatile int *p) { *p = 1; }

static int case_fault_catch_all(void) {
    int r = 0;
    try {
        Guard g('F');
        poke(g_null);
        r = -1;
    } catch (...) {
        r = 31;
    }
    return r;
}

static int case_fault_except(void) {
    int r = 0;
    __try {
        r = *g_null;
        r = -1;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                 : EXCEPTION_CONTINUE_SEARCH) {
        r = 32;
    }
    return r;
}

// The access violation's record says whether the access was a read (0) or a
// write (1), and where: ExceptionInformation[0] and [1].
static unsigned long g_av_kind, g_av_addr;
static int av_filter(EXCEPTION_POINTERS *ep) {
    g_av_kind = ep->ExceptionRecord->ExceptionInformation[0];
    g_av_addr = ep->ExceptionRecord->ExceptionInformation[1];
    return EXCEPTION_EXECUTE_HANDLER;
}

static int case_fault_kind(void) {
    unsigned long read_kind = 9, write_kind = 9;
    int r = 0;
    __try {
        r = *g_null;
    } __except (av_filter(GetExceptionInformation())) {
        read_kind = g_av_kind;
    }
    __try {
        poke(g_null);
    } __except (av_filter(GetExceptionInformation())) {
        write_kind = g_av_kind;
    }
    return read_kind == 0 && write_kind == 1 && g_av_addr == 0;
}

// Static TLS: each thread has its own copy, initialised from the image's template.
__declspec(thread) static int t_value = 5;

static DWORD WINAPI thread_proc(LPVOID p) {
    int *out = (int *)p;
    t_value += 100;
    try {
        throw_int(t_value);
    } catch (int v) {
        *out = v;
    }
    return 0;
}

static int case_thread(void) {
    int got = 0;
    t_value = 1;
    HANDLE h = CreateThread(NULL, 0, thread_proc, &got, 0, NULL);
    if (!h) return 0;
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    return got == 105 && t_value == 1;
}

// GetCurrentThreadId: never 0, a different id on another thread, and the id CreateThread gave out.
static DWORD g_child_id;

static DWORD WINAPI id_proc(LPVOID) {
    g_child_id = GetCurrentThreadId();
    return 0;
}

static int case_thread_ids(void) {
    DWORD created = 0;
    HANDLE h = CreateThread(NULL, 0, id_proc, NULL, 0, &created);
    if (!h) return 0;
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    DWORD me = GetCurrentThreadId();
    return me != 0 && g_child_id != 0 && g_child_id != me && g_child_id == created
        && GetCurrentThreadId() == me;
}

// A thread that ends inside a critical section, by returning or through ExitThread, leaves it
// free: read through the structure's own fields, then entered and left again here.
static CRITICAL_SECTION g_ended_cs;

static DWORD WINAPI enter_twice_and_return(LPVOID) {
    EnterCriticalSection(&g_ended_cs);
    EnterCriticalSection(&g_ended_cs);
    return 0;
}

static DWORD WINAPI enter_and_exit_thread(LPVOID) {
    EnterCriticalSection(&g_ended_cs);
    ExitThread(3);
    return 0;
}

static int case_section_after_thread_end(LPTHREAD_START_ROUTINE body) {
    InitializeCriticalSection(&g_ended_cs);
    HANDLE h = CreateThread(NULL, 0, body, NULL, 0, NULL);
    if (!h) return 0;
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    int freed = g_ended_cs.OwningThread == 0 && g_ended_cs.RecursionCount == 0
        && g_ended_cs.LockCount == -1;
    if (!freed) return 0; // entering it now would wait for ever
    EnterCriticalSection(&g_ended_cs);
    int mine = g_ended_cs.OwningThread == (HANDLE)GetCurrentThreadId()
        && g_ended_cs.RecursionCount == 1 && g_ended_cs.LockCount == 0;
    LeaveCriticalSection(&g_ended_cs);
    DeleteCriticalSection(&g_ended_cs);
    return mine;
}

// A throw out of a callback the system calls (EnumSystemLocalesA's), caught by the caller of the
// system call: the native-to-guest call in between is jumped over. The test host exports the
// machine lock's depth as sfcoptest.dll!LockDepth; it must read the same before and after.
static BOOL CALLBACK throwing_locale(LPSTR) {
    throw_int(61);
    return TRUE;
}

typedef int(WINAPI *DepthFn)(void);
typedef BOOL(WINAPI *EnumLocalesFn)(LOCALE_ENUMPROCA, DWORD);

// /GX assumes an extern "C" function never throws and would drop the try frame around a direct
// call; a call through a pointer may throw.
static EnumLocalesFn volatile g_enumLocales = EnumSystemLocalesA;

static int case_throw_across_callback(void) {
    HMODULE test = LoadLibraryA("sfcoptest");
    DepthFn depth = test ? (DepthFn)GetProcAddress(test, "LockDepth") : 0;
    if (!depth) return 0;
    int before = depth(), caught = 0;
    try {
        g_enumLocales(throwing_locale, LCID_INSTALLED);
    } catch (int v) {
        caught = v;
    }
    return caught == 61 && depth() == before;
}

// A zero divisor in __try: the filter sees EXCEPTION_INT_DIVIDE_BY_ZERO and the __except runs.
static volatile int g_zero;

static int case_divide(void) {
    unsigned long code = 0;
    int r = 1;
    __try {
        r = 100 / g_zero;
    } __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        r = 0;
    }
    return code == EXCEPTION_INT_DIVIDE_BY_ZERO && r == 0;
}

// A mission module's C runtime frees its TLS index when it detaches. The slots are 128; a
// TlsFree that does not give its index back runs them out. This goes through the import table,
// the way a module's TlsFree is resolved, so it fails if another table answers the name first.
static int case_tls_free(void) {
    for (int i = 0; i < 300; i++) {
        DWORD index = TlsAlloc();
        if (index == TLS_OUT_OF_INDEXES) return 0;
        if (!TlsSetValue(index, (LPVOID)(i + 1))) return 0;
        if (!TlsFree(index)) return 0;
    }
    return !TlsFree(9999); // an index that was never allocated is refused
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "uncaught")) {
        printf("ehtest: throwing an int nothing catches\n");
        fflush(stdout);
        throw_int(99);
        return 0;
    }
    check("throw int caught by catch(int)", case_int() == 42);
    check("throw class caught by value", case_class_by_value() == 7);
    check("throw derived class caught by base reference", case_class_by_ref() == 9);
    check("nested try: inner catch skipped, outer catch runs", case_nested() == 5);
    check("rethrow from catch(...) reaches the outer catch", case_rethrow() == 13);
    check("throw from inside a catch reaches the outer catch", case_throw_from_catch() == 20);
    check("unwind through two frames runs destructors in order", case_unwind_two_frames());
    check("100 throws in a loop each caught", case_loop() == 5050);
    check("every exception object destroyed", Error::live == 0);
    check("__try/__except catches RaiseException", case_seh() == 77);
    check("static TLS per thread, and a throw caught on a second thread", case_thread());
    check("thread ids: never 0, distinct per thread, CreateThread's", case_thread_ids());
    check("a thread that returns inside a critical section leaves it free",
          case_section_after_thread_end(enter_twice_and_return));
    check("a thread that calls ExitThread inside a critical section leaves it free",
          case_section_after_thread_end(enter_and_exit_thread));
    g_log[0] = 0;
    check("access violation caught by catch(...), destructor run", case_fault_catch_all() == 31
          && strcmp(g_log, "F") == 0);
    check("access violation caught by __except", case_fault_except() == 32);
    check("access violation records a read and a write", case_fault_kind());
    check("throw across a system callback, lock depth unchanged", case_throw_across_callback());
    check("divide by zero caught by __except", case_divide());
    check("TlsFree gives the index back, 300 alloc/free pairs", case_tls_free());
    printf("ehtest: %d failures\n", g_failures);
    return g_failures;
}
