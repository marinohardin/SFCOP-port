/*
 * Spike host for the lifted StarFleetOP.exe on pcrecomp's win32hle (macOS
 * arm64). Modelled on redalert2-recomp/src/linux/main.c, cut down to what the
 * spike measures: does the lifted program map, bind, run the CRT and reach
 * WinMain and window creation. Not a game host.
 *
 *   sfcop --game DIR            dry run: map + bind, report unresolved imports
 *   sfcop --game DIR --run      enter the CRT entry point
 *         [--headless] [--trace] [--watchdog S]
 *
 * The game folder is mapped as C:. It must be a writable copy, never
 * reference/ (the game writes its settings files).
 */
#define _DARWIN_C_SOURCE
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "win32hle.h"
#include "pe_loader.h"
#include "host.h"

#define SFCOP_IMAGE_BASE 0x00400000u
#define WINMAIN_VA       0x0069A840u

extern const uint32_t sfcop_entry_va;

static unsigned g_watchdog_s = 60;
static struct timespec g_t0;

static double elapsed(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - g_t0.tv_sec) + (t.tv_nsec - g_t0.tv_nsec) / 1e9;
}

/* Called on entry of each function the lift was told to --mark. */
void sfcop_mark(uint32_t va) {
    static uint32_t seen[16];
    for (int i = 0; i < 16; i++) {
        if (seen[i] == va) return;
        if (!seen[i]) {
            seen[i] = va;
            fprintf(stderr, "[mark] reached 0x%08X%s at %.3f s (last import %s)\n", va,
                    va == WINMAIN_VA ? " (WinMain)" : "", elapsed(), g_cur_import ? g_cur_import : "(none)");
            return;
        }
    }
}

/* A catalogued function the lift chose not to emit. With --all there are none. */
void recomp_not_lifted(uint32_t va) {
    fprintf(stderr, "[host] not lifted: sub_%08X\n", va);
    hle_exit(5);
}

/* Guest C++ exceptions: win32hle has no runtime for them (pcrecomp's eh32.c is
 * native32-only, Windows). The landing pads the lift emits need these to link;
 * a frame push/pop is harmless, a catch can never be delivered here. */
#include <setjmp.h>
static __thread recomp_ehframe_t *t_frames;
void recomp_eh_push(recomp_ehframe_t *f) { f->prev = t_frames; t_frames = f; }
void recomp_eh_pop(recomp_ehframe_t *f) {
    for (recomp_ehframe_t *p = t_frames; p; p = p->prev)
        if (p == f) { t_frames = f->prev; return; }
}
recomp_ehframe_t *recomp_eh_owner(uint32_t esp) { (void)esp; return NULL; }
void recomp_eh_resume(recomp_ehframe_t *f, uint32_t resume) {
    (void)f;
    fprintf(stderr, "[host] guest EH resume at 0x%08X: not supported on win32hle\n", resume);
    hle_exit(6);
}

/* OSSpecific95/98.dll: a static constructor (0x006F54E0, tOSVersion) loads one
 * of them by bare name and takes its single export; without it the fallback it
 * installs (0x006F55C0) throws on first use. win32hle neither appends ".dll"
 * nor runs a DLL's code, so the host answers the DLL itself: the export is
 * cdecl InterlockedCompareExchange(dest, exchange, comparand). */
#define OSSPECIFIC_HMODULE 0x7FF00000u
static recomp_func_t hle_original(const char *name) {
    for (const win32hle_shim *s = win32hle_module; s->name; s++)
        if (!strcmp(s->name, name)) return s->fn;
    return NULL;
}
static void sfcop_OS_InterlockedCompareExchange(void) {
    uint32_t p = A32(0), x = A32(1), c = A32(2), old = MEM32(p);
    if (old == c) MEM32(p) = x;
    RET(old, 0);
}
static void sfcop_LoadLibraryA(void) {
    const char *n = ASTR(0);
    if (n && !strncasecmp(n, "OSSpecific", 10)) RET(OSSPECIFIC_HMODULE, 1);
    hle_original("LoadLibraryA")();
}
static void sfcop_GetProcAddress(void) {
    if (A32(0) == OSSPECIFIC_HMODULE && A32(1) >= 0x10000u
        && !strcmp((const char *)hle_ptr(A32(1)), "OS_InterlockedCompareExchange"))
        RET(hle_resolve("OS_InterlockedCompareExchange"), 2);
    hle_original("GetProcAddress")();
}
static const win32hle_shim g_sfcop_shims[] = {
    { "OS_InterlockedCompareExchange", sfcop_OS_InterlockedCompareExchange },
    { "LoadLibraryA", sfcop_LoadLibraryA },
    { "GetProcAddress", sfcop_GetProcAddress },
    { 0, 0 }
};

/* Static TLS (__declspec(thread)): the exe's TLS directory names a template and
 * an index slot; code reads fs:[0x2C] -> slot array -> [index] -> its block.
 * win32hle's TIB has no array, so patches/pcrecomp-tib-hook.patch calls this
 * for every new guest thread. The exe has one TLS block and no callbacks. */
extern void (*hle_tib_hook)(uint32_t tib);
static uint32_t g_tls_start, g_tls_size, g_tls_zero, g_tls_index_va;
static void tls_setup(uint32_t tib) {
    if (!g_tls_size && !g_tls_zero) return;
    uint32_t blk = hle_va(hle_alloc(g_tls_size + g_tls_zero + 16));
    uint32_t arr = hle_va(hle_alloc(64));
    memcpy(hle_ptr(blk), hle_ptr(g_tls_start), g_tls_size);
    memset(hle_ptr(blk + g_tls_size), 0, g_tls_zero);
    memset(hle_ptr(arr), 0, 64);
    MEM32(arr) = blk;
    MEM32(tib + 0x2C) = arr;
}
static void tls_init(const pe_image *img) {
    uint32_t pe = img->base + MEM32(img->base + 0x3C);
    uint32_t rva = MEM32(pe + 0x18 + 0x60 + 9 * 8);     /* DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS] */
    if (!rva) return;
    uint32_t dir = img->base + rva;
    g_tls_start = MEM32(dir);
    g_tls_size = MEM32(dir + 4) - g_tls_start;
    g_tls_index_va = MEM32(dir + 8);
    g_tls_zero = MEM32(dir + 16);
    MEM32(g_tls_index_va) = 0;
    hle_tib_hook = tls_setup;
    fprintf(stderr, "[host] static TLS: template 0x%08X, %u bytes + %u zero, index at 0x%08X, callbacks at 0x%08X\n",
            g_tls_start, g_tls_size, g_tls_zero, g_tls_index_va, MEM32(dir + 12));
}

static void report(const char *why) {
    fprintf(stderr, "\n=== %s at %.3f s ===\n  in lifted sub_%08X, last import %s\n", why, elapsed(), g_cur_func,
            g_cur_import ? g_cur_import : "(none)");
    fprintf(stderr, "  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
    fprintf(stderr, "last indirect calls (newest first):\n");
    for (int i = 1; i <= 24 && i <= (int)g_icall_trace_idx; i++) {
        uint32_t k = (g_icall_trace_idx - i) & (ICALL_TRACE_SIZE - 1);
        const char *nm = hle_name(g_icall_trace[k]);
        fprintf(stderr, "  0x%08X  from 0x%08X  %s\n", g_icall_trace[k], g_icall_from[k], nm ? nm : "");
    }
}

static void on_fault(int sig, siginfo_t *si, void *uc) {
    static volatile int once;
    (void)uc;
    if (__sync_lock_test_and_set(&once, 1)) _exit(3);
    char why[96];
    snprintf(why, sizeof why, "fault: signal %d, address %p", sig, si->si_addr);
    report(why);
    fflush(stderr);
    _exit(3);
}

static void *watchdog(void *arg) {
    (void)arg;
    sleep(g_watchdog_s);
    report("watchdog");
    fflush(stderr);
    hle_exit(4);
    return NULL;
}

int main(int argc, char **argv) {
    const char *game = NULL;
    int run = 0, headless = 0;
    clock_gettime(CLOCK_MONOTONIC, &g_t0);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--run")) run = 1;
        else if (!strcmp(argv[i], "--headless")) headless = 1;
        else if (!strcmp(argv[i], "--trace")) win32hle_trace = 1;
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) g_watchdog_s = (unsigned)atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: sfcop --game DIR [--run] [--headless] [--trace] [--watchdog S]\n");
            return 64;
        }
    }
    if (!game) { fprintf(stderr, "--game DIR is required (a writable copy of the install)\n"); return 64; }
    char game_full[PATH_MAX], exe_host[PATH_MAX + 16];
    if (!realpath(game, game_full)) { fprintf(stderr, "no game folder at %s\n", game); return 1; }
    if (strstr(game_full, "/reference/")) { fprintf(stderr, "refusing to run from reference/: copy it\n"); return 1; }
    hle_set_drive('C', game_full);
    if (!hle_host_path("C:\\StarFleetOP.exe", exe_host, sizeof exe_host)) {
        fprintf(stderr, "no StarFleetOP.exe in %s\n", game_full);
        return 1;
    }

    {
        static uint8_t alt[1 << 16];
        stack_t ss = { .ss_sp = alt, .ss_size = sizeof alt };
        sigaltstack(&ss, NULL);
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_fault;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
        sigaction(SIGFPE, &sa, NULL);
        sigaction(SIGILL, &sa, NULL);
    }

    win32hle_register(g_sfcop_shims);   /* first: the host's answers win */
    recomp_host_init();
    printf("StarFleetOP spike host (win32hle, %s)\n  lifted functions in dispatch: %u\n",
#ifdef __aarch64__
           "arm64",
#else
           "x86-64",
#endif
           recomp_dispatch_count);
    pe_image img;
    if (recomp_pe_map(exe_host, &img) != 0 || img.base != SFCOP_IMAGE_BASE) {
        fprintf(stderr, "cannot map %s at 0x%08X\n", exe_host, SFCOP_IMAGE_BASE);
        return 1;
    }
    hle_module_add("C:\\StarFleetOP.exe", img.base, img.resource_rva, img.resource_size, 1);
    int unresolved = recomp_pe_bind(&img, hle_resolve_or_stub);
    printf("  mapped %s: 0x%08X-0x%08X, %d imports bound to not-implemented stubs\n",
           exe_host, img.base, img.base + img.span, unresolved);
    hle_set_command_line("\"C:\\StarFleetOP.exe\"");
    tls_init(&img);
    if (!run) {
        printf("\n(dry run: image mapped and bound; --run enters 0x%08X)\n", sfcop_entry_va);
        return 0;
    }
    if (chdir(game_full) != 0) { fprintf(stderr, "cannot enter %s\n", game_full); return 1; }
    if (hle_screen_open("Starfleet Command: Orion Pirates (lifted spike)", 0, headless) != 0) return 1;
    if (g_watchdog_s) {
        pthread_t t;
        pthread_create(&t, NULL, watchdog, NULL);
        pthread_detach(t);
    }
    printf("  entering 0x%08X\n", sfcop_entry_va);
    fflush(stdout);
    uint32_t rc = hle_call_guest(sfcop_entry_va, 0, NULL);
    fprintf(stderr, "\nentry returned eax=%08X at %.3f s\n", rc, elapsed());
    hle_exit((int)rc);
    return 0;
}
