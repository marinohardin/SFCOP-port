// Guest-side C++ exception tests for the lifted build (task P1-1).
//
// Built with the VC6 SP4 compiler in toolchain/ (/GX, static CRT), lifted with the
// same driver as the game and run on win32hle. Each case prints one PASS or FAIL
// line; the exit code is the number of failures. Expected values are written out.
// `ehtest uncaught` throws an int nothing catches: the host must report it and exit 7.
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
    g_log[0] = 0;
    check("access violation caught by catch(...), destructor run", case_fault_catch_all() == 31
          && strcmp(g_log, "F") == 0);
    check("access violation caught by __except", case_fault_except() == 32);
    check("access violation records a read and a write", case_fault_kind());
    printf("ehtest: %d failures\n", g_failures);
    return g_failures;
}
