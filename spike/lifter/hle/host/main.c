/*
 * P1-2 host for the lifted StarFleetOP.exe on pcrecomp's win32hle (macOS
 * arm64): the P0-4 spike host plus the HLE modules in ../src (DirectInput,
 * Miles samples, DirectSound3D, wide-char file calls, small stubs, the
 * game's OSSpecific DLL, static constructor and throw logging), and
 * screenshots and a frame counter for the report.
 *
 *   sfcop --game DIR            dry run: map + bind, report unresolved imports
 *   sfcop --game DIR --run      enter the CRT entry point
 *         [--headless] [--trace] [--watchdog S] [--silent]
 *         [--shots DIR] [--shot-every S]
 *
 * --silent makes the Miles samples silent (their timing still runs); a
 * headless run is always silent. --shots writes the primary surface as BMP
 * every --shot-every seconds (default 5) and once more at the watchdog.
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
#include <mach-o/dyld.h>

#include "win32hle.h"
#include "pe_loader.h"
#include "host.h"
#include "sfcop_hle.h"

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

/* ---- screenshots and frame rate ----
 * The primary surface (or, before DirectDraw, the GDI window surface) as a
 * 24-bit BMP. 8-bit uses the primary's palette; 16-bit is RGB565. */
static const char *g_shots;
static unsigned g_shot_every = 5;
static void write_bmp(const char *path, const uint8_t *px, int w, int h, int pitch, int bpp, const uint32_t *pal) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[shot] cannot write %s\n", path); return; }
    int row = (w * 3 + 3) & ~3;
    uint32_t size = 54u + (uint32_t)row * (uint32_t)h;
    uint8_t hd[54] = { 'B', 'M' };
    memcpy(hd + 2, &size, 4);
    hd[10] = 54, hd[14] = 40;
    memcpy(hd + 18, &w, 4), memcpy(hd + 22, &h, 4);
    hd[26] = 1, hd[28] = 24;
    fwrite(hd, 1, 54, f);
    uint8_t *line = calloc(1, (size_t)row);
    for (int y = h - 1; y >= 0; y--) {
        const uint8_t *src = px + (size_t)y * (size_t)pitch;
        for (int x = 0; x < w; x++) {
            uint32_t r = 0, g = 0, b = 0;
            if (bpp == 8) { uint32_t c = pal ? pal[src[x]] : (uint32_t)src[x] * 0x010101u; r = c & 255, g = c >> 8 & 255, b = c >> 16 & 255; }
            else if (bpp == 16) { uint16_t c = (uint16_t)(src[2 * x] | src[2 * x + 1] << 8); r = (c >> 11) * 255 / 31, g = (c >> 5 & 63) * 255 / 63, b = (c & 31) * 255 / 31; }
            else if (bpp == 24) { b = src[3 * x], g = src[3 * x + 1], r = src[3 * x + 2]; }
            else { b = src[4 * x], g = src[4 * x + 1], r = src[4 * x + 2]; }
            line[3 * x] = (uint8_t)b, line[3 * x + 1] = (uint8_t)g, line[3 * x + 2] = (uint8_t)r;
        }
        fwrite(line, 1, (size_t)row, f);
    }
    free(line);
    fclose(f);
}
static int shoot(const char *tag) {
    const uint8_t *px;
    const uint32_t *pal = NULL;
    int w, h, pitch, bpp;
    unsigned seq;
    char path[PATH_MAX];
    int dd = hle_dd_frame(&px, &w, &h, &pitch, &bpp, &pal, &seq);
    if (!dd && !hle_gdi_window_frame(&px, &w, &h, &pitch, &bpp, &seq)) return 0;
    if (!px || w <= 0 || h <= 0) return 0;
    snprintf(path, sizeof path, "%s/%s_%07.2fs.bmp", g_shots, tag, elapsed());
    write_bmp(path, px, w, h, pitch, bpp, pal);
    fprintf(stderr, "[shot] %s (%dx%d, %d bpp, %s)\n", path, w, h, bpp, dd ? "DirectDraw primary" : "GDI window");
    return 1;
}
/* In a windowed run the presenter hands over each frame it shows: shots are
 * taken there, on the game's thread, so they are whole frames, and the
 * shown frames are the frame rate. Headless, the stats thread reads the
 * primary directly (it can catch a frame half redrawn). */
static int g_windowed;
static volatile int g_shot_pending;
static volatile unsigned g_frames_shown;
static void on_frame(const uint8_t *px, int w, int h, int pitch, int bpp) {
    g_frames_shown++;
    if (!g_shot_pending || !g_shots) return;
    const uint8_t *p0;
    const uint32_t *pal = NULL;
    int w0, h0, pitch0, bpp0;
    unsigned seq;
    if (bpp == 8) hle_dd_frame(&p0, &w0, &h0, &pitch0, &bpp0, &pal, &seq);
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s_%07.2fs.bmp", g_shots, g_shot_pending == 2 ? "watchdog" : "frame", elapsed());
    write_bmp(path, px, w, h, pitch, bpp, pal);
    fprintf(stderr, "[shot] %s (%dx%d, %d bpp, shown frame)\n", path, w, h, bpp);
    g_shot_pending = 0;
}
static volatile unsigned g_primary_changes;
static void (*g_primary_next)(void);
static void count_primary(void) {
    g_primary_changes++;
    if (g_primary_next) g_primary_next();
}
static void *stats(void *arg) {
    (void)arg;
    unsigned last = 0, last_shown = 0, t = 0;
    for (;;) {
        sleep(1);
        t++;
        unsigned now = g_primary_changes, shown = g_frames_shown;
        if (now != last || shown != last_shown)
            fprintf(stderr, "[fps] %.1f s: %u frames shown, %u primary updates in the last second\n", elapsed(),
                    shown - last_shown, now - last);
        last = now, last_shown = shown;
        if (g_shots && t % g_shot_every == 0) { if (g_windowed) g_shot_pending = 1; else shoot("t"); }
    }
    return NULL;
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
    if (__sync_lock_test_and_set(&once, 1)) _exit(3);
    char why[96];
    snprintf(why, sizeof why, "fault: signal %d, address %p", sig, si->si_addr);
    report(why);
#ifdef __aarch64__
    /* for: atos -o build/sfcop -l <load> <pc> */
    fprintf(stderr, "  host pc 0x%llx lr 0x%llx, image loaded at %p\n",
            (unsigned long long)((ucontext_t *)uc)->uc_mcontext->__ss.__pc,
            (unsigned long long)((ucontext_t *)uc)->uc_mcontext->__ss.__lr, (void *)_dyld_get_image_header(0));
#endif
    fflush(stderr);
    _exit(3);
}

static void *watchdog(void *arg) {
    (void)arg;
    sleep(g_watchdog_s);
    if (g_shots && g_windowed) { g_shot_pending = 2; usleep(200000); }
    else if (g_shots) shoot("watchdog");
    report("watchdog");
    fflush(stderr);
    hle_exit(4);
    return NULL;
}

int main(int argc, char **argv) {
    const char *game = NULL;
    int run = 0, headless = 0, silent = 0;
    clock_gettime(CLOCK_MONOTONIC, &g_t0);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--run")) run = 1;
        else if (!strcmp(argv[i], "--headless")) headless = 1;
        else if (!strcmp(argv[i], "--trace")) win32hle_trace = 1;
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) g_watchdog_s = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--silent")) silent = 1;
        else if (!strcmp(argv[i], "--shots") && i + 1 < argc) g_shots = argv[++i];
        else if (!strcmp(argv[i], "--shot-every") && i + 1 < argc) g_shot_every = (unsigned)atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: sfcop --game DIR [--run] [--headless] [--trace] [--watchdog S] [--silent] [--shots DIR] [--shot-every S]\n");
            return 64;
        }
    }
    if (!game) { fprintf(stderr, "--game DIR is required (a writable copy of the install)\n"); return 64; }
    static char shots_full[PATH_MAX];                    /* the run enters the game folder: keep it absolute */
    if (g_shots) {
        if (!realpath(g_shots, shots_full)) { fprintf(stderr, "no screenshot folder at %s\n", g_shots); return 1; }
        g_shots = shots_full;
    }
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

    /* first: ours win where win32hle answers the same name */
    win32hle_register(sfcop_hle_game);
    win32hle_register(sfcop_hle_dinput);
    win32hle_register(sfcop_hle_mss);
    win32hle_register(sfcop_hle_ds3d);
    win32hle_register(sfcop_hle_widefile);
    win32hle_register(sfcop_hle_misc);
    recomp_host_init();
    sfcop_game_init();
    sfcop_mss_silent = silent || headless;
    if (!g_shot_every) g_shot_every = 5;
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
    if (hle_screen_open("Starfleet Command: Orion Pirates (lifted, P1-2)", 0, headless) != 0) return 1;
    g_windowed = !headless;
    hle_screen_frame_hook = on_frame;
    g_primary_next = hle_dd_primary_hook;
    hle_dd_primary_hook = count_primary;
    {
        pthread_t t;
        pthread_create(&t, NULL, stats, NULL);
        pthread_detach(t);
    }
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
