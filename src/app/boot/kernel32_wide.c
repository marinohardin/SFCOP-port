// TEMPORARY: replaced by Wave 1 B5 (store, kernel and registry)
/*
 * kernel32_wide.c - the wide-character (UTF-16) KERNEL32 file and directory
 * calls, over win32hle's ANSI ones. Upstreamable: yes.
 *
 * Each W shim converts its string arguments to UTF-8 copies in this thread's
 * guest scratch, writes their addresses over the arguments on the guest
 * stack, and runs the A shim, which reads them and pops the same number of
 * arguments. Output strings go through a scratch buffer and are widened into
 * the caller's afterwards; counts are returned in characters, as the W calls
 * count.
 */
#include <stdio.h>
#include <string.h>
#include "sfcop_hle.h"

#define ARG_SLOT(i) (g_esp + 4u + 4u * (uint32_t)(i))
#define PATHN 1024

static void run_a(const char *name) {
    recomp_func_t f = sfcop_hle_original(name);
    if (!f) hle_fatal("kernel32_wide: win32hle has no %s", name);
    f();
}
/* Replace wide-string argument i with a UTF-8 scratch copy (NULL stays NULL). */
static void narrow_arg(int i) {
    uint32_t w = MEM32(ARG_SLOT(i));
    if (!w) return;
    char tmp[PATHN];
    sfcop_wide_to_utf8(w, tmp, sizeof tmp);
    MEM32(ARG_SLOT(i)) = hle_scratch_copy(tmp, (uint32_t)strlen(tmp) + 1);
}
/* A call whose only string argument is one wide input. */
static void narrowed(const char *aname, int arg) {
    uint32_t mark = hle_scratch_mark();
    narrow_arg(arg);
    run_a(aname);
    hle_scratch_release(mark);
}
#define NARROW(fn, aname) static void fn(void) { narrowed(aname, 0); }
NARROW(w_CreateFileW, "CreateFileA")
NARROW(w_CreateDirectoryW, "CreateDirectoryA")
NARROW(w_RemoveDirectoryW, "RemoveDirectoryA")
NARROW(w_DeleteFileW, "DeleteFileA")
NARROW(w_GetFileAttributesW, "GetFileAttributesA")
NARROW(w_SetFileAttributesW, "SetFileAttributesA")
NARROW(w_SetCurrentDirectoryW, "SetCurrentDirectoryA")
NARROW(w_GetDriveTypeW, "GetDriveTypeA")
NARROW(w_GetDiskFreeSpaceW, "GetDiskFreeSpaceA")

/* (buffer, size) or (size, buffer) -> characters written, or the size needed
 * with the terminator when it does not fit. */
static void dir_query(const char *aname, int buf_arg, int size_arg) {
    uint32_t wbuf = MEM32(ARG_SLOT(buf_arg)), n = MEM32(ARG_SLOT(size_arg));
    uint32_t mark = hle_scratch_mark(), tmp = hle_scratch(PATHN);
    MEM32(ARG_SLOT(buf_arg)) = tmp, MEM32(ARG_SLOT(size_arg)) = PATHN;
    run_a(aname);
    uint32_t len = g_eax;
    if (len && len < PATHN) {
        uint32_t probe = hle_scratch(2 * PATHN + 2);
        uint32_t need = sfcop_utf8_to_wide((const char *)hle_ptr(tmp), probe, PATHN + 1);
        if (need + 1 > n || !wbuf) g_eax = need + 1;
        else g_eax = sfcop_utf8_to_wide((const char *)hle_ptr(tmp), wbuf, n);
    }
    hle_scratch_release(mark);
}
static void w_GetCurrentDirectoryW(void) { dir_query("GetCurrentDirectoryA", 1, 0); }
static void w_GetWindowsDirectoryW(void) { dir_query("GetWindowsDirectoryA", 0, 1); }
static void w_GetSystemDirectoryW(void) { dir_query("GetSystemDirectoryA", 0, 1); }

/* (root, volname, volsize, &serial, &maxlen, &flags, fsname, fssize) */
static void w_GetVolumeInformationW(void) {
    uint32_t vol = MEM32(ARG_SLOT(1)), voln = MEM32(ARG_SLOT(2)), fs = MEM32(ARG_SLOT(6)), fsn = MEM32(ARG_SLOT(7));
    uint32_t mark = hle_scratch_mark();
    narrow_arg(0);
    uint32_t tv = vol ? hle_scratch(260) : 0, tf = fs ? hle_scratch(260) : 0;
    MEM32(ARG_SLOT(1)) = tv, MEM32(ARG_SLOT(2)) = vol ? 260 : 0;
    MEM32(ARG_SLOT(6)) = tf, MEM32(ARG_SLOT(7)) = fs ? 260 : 0;
    run_a("GetVolumeInformationA");
    if (g_eax && vol) sfcop_utf8_to_wide((const char *)hle_ptr(tv), vol, voln);
    if (g_eax && fs) sfcop_utf8_to_wide((const char *)hle_ptr(tf), fs, fsn);
    hle_scratch_release(mark);
}

/* WIN32_FIND_DATAA (320 bytes) -> WIN32_FIND_DATAW (592): the first 44 bytes
 * agree; the names follow, 260 and 14 characters. */
static void find_data_widen(uint32_t a, uint32_t w) {
    memcpy(hle_ptr(w), hle_ptr(a), 44);
    char name[261], alt[15];
    memcpy(name, hle_ptr(a + 44), 260), name[260] = 0;
    memcpy(alt, hle_ptr(a + 304), 14), alt[14] = 0;
    sfcop_utf8_to_wide(name, w + 44, 260);
    sfcop_utf8_to_wide(alt, w + 564, 14);
}
static void w_FindFirstFileW(void) {                     /* (pattern, &data) */
    uint32_t w = MEM32(ARG_SLOT(1));
    uint32_t mark = hle_scratch_mark(), a = hle_scratch(320);
    narrow_arg(0);
    MEM32(ARG_SLOT(1)) = a;
    run_a("FindFirstFileA");
    if (g_eax != 0xFFFFFFFFu && w) find_data_widen(a, w);
    hle_scratch_release(mark);
}
static void w_FindNextFileW(void) {                      /* (handle, &data) */
    uint32_t w = MEM32(ARG_SLOT(1));
    uint32_t mark = hle_scratch_mark(), a = hle_scratch(320);
    MEM32(ARG_SLOT(1)) = a;
    run_a("FindNextFileA");
    if (g_eax && w) find_data_widen(a, w);
    hle_scratch_release(mark);
}

const win32hle_shim sfcop_spike_widefile[] = {
    { "CreateFileW", w_CreateFileW }, { "CreateDirectoryW", w_CreateDirectoryW },
    { "RemoveDirectoryW", w_RemoveDirectoryW }, { "DeleteFileW", w_DeleteFileW },
    { "GetFileAttributesW", w_GetFileAttributesW }, { "SetFileAttributesW", w_SetFileAttributesW },
    { "SetCurrentDirectoryW", w_SetCurrentDirectoryW }, { "GetCurrentDirectoryW", w_GetCurrentDirectoryW },
    { "GetWindowsDirectoryW", w_GetWindowsDirectoryW }, { "GetSystemDirectoryW", w_GetSystemDirectoryW },
    { "GetDriveTypeW", w_GetDriveTypeW }, { "GetDiskFreeSpaceW", w_GetDiskFreeSpaceW },
    { "GetVolumeInformationW", w_GetVolumeInformationW },
    { "FindFirstFileW", w_FindFirstFileW }, { "FindNextFileW", w_FindNextFileW },
    { 0, 0 }
};
