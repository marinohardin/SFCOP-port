// TEMPORARY: replaced by Wave 1 B6 (guest runtime)
/*
 * sfcop_game.c - what only StarFleetOP.exe needs. Upstreamable: no.
 *
 * 1. OSSpecific95/98.dll. A static constructor (0x006F54E0, tOSVersion)
 *    loads one of them by bare name and takes its single export; without it
 *    the fallback it installs (0x006F55C0) throws on first use. win32hle
 *    neither appends ".dll" nor runs a DLL's code, and the DLL holds one
 *    function, so the host is the DLL: the export is cdecl
 *    OS_InterlockedCompareExchange(dest, exchange, comparand).
 *
 * 2. The static constructor at 0x0059644A. It is reachable only through the
 *    CRT's _initterm table, so the catalogue missed it and the lift has no
 *    function there (the spike's "ICALL unresolved ... from 0x0073B74B").
 *    Its bytes do what 0x004014A9's do, constant for constant: set bit 0 of
 *    the guard at 0x00C8193C, then atexit(0x004BD431). It is one of the
 *    duplicate initialisers MSVC emits for a guarded static, so the lifted
 *    0x004014A9 runs in its place (patches/pcrecomp-manual-lookup-hook.patch).
 *
 * 3. Guest C++ throws. Until guest exception handling lands (P1-1), a throw
 *    reaches RaiseException(0xE06D7363), where win32hle ends the run. This
 *    logs what was thrown first: every catchable type name from the
 *    ThrowInfo, the object, the innermost guest EH frame's handler stub (the
 *    function that would catch or unwind first) and the recent indirect
 *    calls. It does not try to continue.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "sfcop_hle.h"

extern uint32_t g_fs_base;
extern recomp_func_t (*hle_manual_hook)(uint32_t va);
extern void sub_004014A9(void);

/* ---- 1. OSSpecific95/98.dll ---- */
#define OSSPECIFIC_HMODULE 0x7FF00000u
static void os_InterlockedCompareExchange(void) {        /* cdecl: the caller pops */
    uint32_t p = A32(0), x = A32(1), c = A32(2), old = MEM32(p);
    if (old == c) MEM32(p) = x;
    RET(old, 0);
}
static void k_LoadLibraryA(void) {
    const char *n = ASTR(0);
    if (n && !strncasecmp(n, "OSSpecific", 10)) RET(OSSPECIFIC_HMODULE, 1);
    sfcop_hle_original("LoadLibraryA")();
}
static void k_GetProcAddress(void) {
    if (A32(0) == OSSPECIFIC_HMODULE && A32(1) >= 0x10000u
        && !strcmp((const char *)hle_ptr(A32(1)), "OS_InterlockedCompareExchange"))
        RET(hle_resolve("OS_InterlockedCompareExchange"), 2);
    sfcop_hle_original("GetProcAddress")();
}
static void k_FreeLibrary(void) {
    if (A32(0) == OSSPECIFIC_HMODULE) RET(1, 1);
    sfcop_hle_original("FreeLibrary")();
}

/* ---- 2. the constructor the catalogue missed ---- */
static recomp_func_t manual(uint32_t va) {
    return va == 0x0059644Au ? sub_004014A9 : NULL;
}

/* ---- 3. guest throws ---- */
static void k_RaiseException(void) {                     /* (code, flags, nargs, args) */
    uint32_t code = A32(0), nargs = A32(2), args = A32(3);
    fprintf(stderr, "\n[throw] RaiseException(0x%08X) in lifted sub_%08X\n", code, g_cur_func);
    if (code == 0xE06D7363u && nargs >= 3 && args) {
        uint32_t obj = MEM32(args + 4), ti = MEM32(args + 8);
        fprintf(stderr, "[throw] C++ object 0x%08X, ThrowInfo 0x%08X\n", obj, ti);
        uint32_t cta = ti ? MEM32(ti + 12) : 0, n = cta ? MEM32(cta) : 0;
        for (uint32_t i = 0; i < n && i < 16; i++) {
            uint32_t ct = MEM32(cta + 4 + 4 * i), td = ct ? MEM32(ct + 4) : 0;
            fprintf(stderr, "[throw]   catchable %u: %s\n", i, td ? (const char *)hle_ptr(td + 8) : "(null)");
        }
    }
    uint32_t rec = g_fs_base ? MEM32(g_fs_base) : 0;
    for (int depth = 0; rec && rec != 0xFFFFFFFFu && depth < 6; depth++, rec = MEM32(rec)) {
        uint32_t h = MEM32(rec + 4);
        /* a VC6 frame handler stub: mov eax, FuncInfo ; jmp __CxxFrameHandler */
        uint32_t fi = MEM8(h) == 0xB8 ? MEM32(h + 1) : 0;
        fprintf(stderr, "[throw] EH frame %d at 0x%08X: handler 0x%08X%s", depth, rec, h, fi ? "" : "\n");
        if (fi) fprintf(stderr, " (FuncInfo 0x%08X, %u try blocks)\n", fi, MEM32(fi + 12));
    }
    fprintf(stderr, "[throw] last indirect calls (newest first):\n");
    for (int i = 1; i <= 12 && i <= (int)g_icall_trace_idx; i++) {
        uint32_t k = (g_icall_trace_idx - i) & (ICALL_TRACE_SIZE - 1);
        const char *nm = hle_name(g_icall_trace[k]);
        fprintf(stderr, "[throw]   0x%08X from 0x%08X %s\n", g_icall_trace[k], g_icall_from[k], nm ? nm : "");
    }
    fprintf(stderr, "[throw] guest C++ exceptions are not handled yet (Wave 1 B6): stopping\n");
    hle_exit(1);   /* the spike exited 3, which DESIGN 6.8 gives to a failed install check */
}

/* ---- diagnostics: a wait that fails ---- */
static void k_WaitForSingleObject(void) {
    uint32_t h = A32(0), ms = A32(1);
    sfcop_hle_original("WaitForSingleObject")();
    if (g_eax == 0xFFFFFFFFu)
        fprintf(stderr, "[wait] WaitForSingleObject(0x%08X, %u) failed in sub_%08X: handle type %d\n", h, ms, g_cur_func, hle_handle_type(h));
}

/* ---- diagnostics: the space background the boot picked ----
 * The game seeds its random numbers from the clock, and the first draws pick one of the space
 * backgrounds (assets\models\space\spaceNN.mod) that the boot loads. Which one it is decides
 * whether a boot on a damaged install reaches the CD-key prompt (src/app/boot/results/README.md),
 * so it is logged. */
static void k_CreateFileA(void) {
    const char *n = ASTR(0);
    if (n && strcasestr(n, "models\\space\\space")) fprintf(stderr, "[file] %s\n", n);
    sfcop_hle_original("CreateFileA")();
}

void sfcop_game_init(void) {
    hle_manual_hook = manual;
}

const win32hle_shim sfcop_spike_game[] = {
    { "OS_InterlockedCompareExchange", os_InterlockedCompareExchange },
    { "LoadLibraryA", k_LoadLibraryA }, { "GetProcAddress", k_GetProcAddress }, { "FreeLibrary", k_FreeLibrary },
    { "RaiseException", k_RaiseException },
    { "WaitForSingleObject", k_WaitForSingleObject },
    { "CreateFileA", k_CreateFileA },
    { 0, 0 }
};
