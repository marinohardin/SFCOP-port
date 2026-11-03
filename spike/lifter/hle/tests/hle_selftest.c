/*
 * hle_selftest.c - checks the P1-2 HLE modules through the shim ABI, with no
 * game binary, no display and no audio device: each shim is entered with
 * its stdcall arguments on the guest stack, and every call checks the
 * result and that the shim popped exactly its arguments.
 *
 * Expected values are written out by hand. The Winsock part uses loopback
 * sockets only. Build and run: ../scripts/selftest.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "win32hle.h"
#include "sfcop_hle.h"

const recomp_dispatch_entry_t recomp_dispatch_table[] = { { 0, 0 } };
const uint32_t recomp_dispatch_count = 0;

static int fails, checks;
static void ok(const char *what, int cond) {
    checks++;
    if (!cond) { printf("FAIL %s\n", what); fails++; }
}

/* Enter a shim with n stdcall arguments; check it popped them. */
static uint32_t call_fn(const char *what, recomp_func_t fn, int n, const uint32_t *a) {
    uint32_t esp0 = g_esp;
    g_esp -= 4u * (uint32_t)n;
    for (int i = 0; i < n; i++) MEM32(g_esp + 4u * (uint32_t)i) = a[i];
    g_esp -= 4;
    MEM32(g_esp) = 0;                                    /* the return address */
    fn();
    if (g_esp != esp0) { printf("FAIL %s: popped %d bytes, not %d\n", what, (int)(g_esp - (esp0 - 4u * (uint32_t)n - 4u)), 4 * n + 4); fails++; }
    checks++;
    g_esp = esp0;
    return g_eax;
}
/* A shim by name from one of our tables, or (t NULL) from win32hle's. */
static recomp_func_t find(const win32hle_shim *t, const char *name) {
    if (!t) { recomp_func_t f = sfcop_hle_original(name); if (f) return f; }
    for (; t && t->name; t++) if (!strcmp(t->name, name)) return t->fn;
    printf("FAIL no shim %s\n", name);
    fails++;
    exit(1);
}
#define CALL(table, name, ...) \
    call_fn(name, find(table, name), (int)(sizeof((uint32_t[]){ __VA_ARGS__ }) / 4), (uint32_t[]){ __VA_ARGS__ })
#define CALL0(table, name) call_fn(name, find(table, name), 0, NULL)
/* A COM method: slot of obj's vtable, this first. */
static uint32_t vcall(uint32_t obj, int slot, int n, const uint32_t *a) {
    uint32_t args[12] = { obj };
    for (int i = 0; i < n; i++) args[1 + i] = a[i];
    recomp_func_t fn = recomp_lookup_import(MEM32(MEM32(obj) + 4u * (uint32_t)slot));
    if (!fn) { printf("FAIL slot %d is not a shim\n", slot); fails++; return 0xFFFFFFFFu; }
    char what[32];
    snprintf(what, sizeof what, "vtable slot %d", slot);
    return call_fn(what, fn, n + 1, args);
}
#define VCALL(obj, slot, ...) vcall(obj, slot, (int)(sizeof((uint32_t[]){ __VA_ARGS__ }) / 4), (uint32_t[]){ __VA_ARGS__ })
#define VCALL0(obj, slot) vcall(obj, slot, 0, NULL)

static uint32_t gbuf(uint32_t n) { return hle_va(hle_alloc(n)); }
static uint32_t gstr(const char *s) { return hle_guest_strdup(s); }
static uint32_t gwstr(const char *s) {
    uint32_t w = gbuf(2 * (uint32_t)strlen(s) + 2);
    for (size_t i = 0; s[i]; i++) MEM16(w + 2 * i) = (uint8_t)s[i];
    return w;
}
static uint32_t f32(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static const uint8_t GUID_SysKeyboard[16] = { 0x61, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11,
                                              0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
static const uint8_t GUID_SysMouse[16] = { 0x60, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11,
                                           0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
static const uint8_t IID_3DBuffer[16] = { 0x86, 0xFA, 0x9A, 0x27, 0x81, 0x49, 0xCE, 0x11,
                                          0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 };
static const uint8_t IID_3DListener[16] = { 0x84, 0xFA, 0x9A, 0x27, 0x81, 0x49, 0xCE, 0x11,
                                            0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 };

static void test_dinput(void) {
    uint32_t out = gbuf(4), kb = gbuf(16), mouse = gbuf(16), state = gbuf(256), fmt = gbuf(24);
    memcpy(hle_ptr(kb), GUID_SysKeyboard, 16);
    memcpy(hle_ptr(mouse), GUID_SysMouse, 16);
    ok("DirectInputCreateA succeeds", CALL(sfcop_hle_dinput, "DirectInputCreateA", 0x400000, 0x700, out, 0) == 0);
    uint32_t di = MEM32(out);
    ok("an IDirectInput", di != 0);
    ok("CreateDevice(mouse) is not served", VCALL(di, 3, mouse, out, 0) == 0x80040154u && MEM32(out) == 0);
    ok("CreateDevice(keyboard) succeeds", VCALL(di, 3, kb, out, 0) == 0);
    uint32_t dev = MEM32(out);
    MEM32(fmt + 12) = 256;                               /* c_dfDIKeyboard's dwDataSize */
    ok("SetDataFormat", VCALL(dev, 11, fmt) == 0);
    ok("SetCooperativeLevel", VCALL(dev, 13, 0x1234, 0x16) == 0);
    ok("GetDeviceState before Acquire: not acquired", VCALL(dev, 9, 256, state) == 0x8007000Cu);
    ok("Acquire", VCALL0(dev, 7) == 0);
    ok("Acquire again: no effect", VCALL0(dev, 7) == 1);
    memset(hle_ptr(state), 0xAA, 256);
    ok("GetDeviceState", VCALL(dev, 9, 256, state) == 0);
    int zero = 1;
    for (int i = 0; i < 256; i++) if (MEM8(state + (uint32_t)i)) zero = 0;
    ok("no key is down", zero);
    uint32_t n = gbuf(4), dod = gbuf(16 * 8), prop = gbuf(20);
    MEM32(n) = 8;
    ok("GetDeviceData without a buffer fails", VCALL(dev, 10, 16, dod, n, 0) == 0x8007000Cu);
    MEM32(prop) = 20, MEM32(prop + 4) = 16, MEM32(prop + 16) = 32;
    ok("SetProperty(DIPROP_BUFFERSIZE)", VCALL(dev, 6, 1, prop) == 0);
    MEM32(prop + 16) = 0;
    ok("GetProperty(DIPROP_BUFFERSIZE) is 32", VCALL(dev, 5, 1, prop) == 0 && MEM32(prop + 16) == 32);
    ok("GetDeviceData: no events", VCALL(dev, 10, 16, dod, n, 0) == 0 && MEM32(n) == 0);
    uint32_t caps = gbuf(44);
    MEM32(caps) = 44;
    ok("GetCapabilities: a PC enhanced keyboard", VCALL(dev, 3, caps) == 0 && MEM32(caps + 8) == 0x403 && MEM32(caps + 16) == 128);
    ok("Unacquire", VCALL0(dev, 8) == 0);
    ok("Release the device", VCALL0(dev, 2) == 0);
    ok("Release the interface", VCALL0(di, 2) == 0);
}

static void test_mss(void) {
    /* the hand-decoded IMA block: first sample 0, index 0, then 0x07:
     * nibble 7 at step 7 gives +11 (index 8), nibble 0 at step 16 gives +2 */
    static const uint8_t block[5] = { 0x00, 0x00, 0x00, 0x00, 0x07 };
    int16_t *pcm = NULL;
    uint32_t frames = sfcop_mss_decode_ima(block, 5, 5, 1, &pcm);
    ok("IMA block decodes to 3 frames", frames == 3);
    ok("IMA samples 0, 11, 13", pcm && pcm[0] == 0 && pcm[1] == 11 && pcm[2] == 13);
    free(pcm);

    sfcop_mss_manual_clock = 1;
    uint32_t drv = gbuf(4), wo = gbuf(4);
    ok("AIL_waveOutOpen", CALL(sfcop_hle_mss, "_AIL_waveOutOpen@16", drv, wo, 0xFFFFFFFFu, 0) == 0 && MEM32(drv));
    uint32_t s = CALL(sfcop_hle_mss, "_AIL_allocate_sample_handle@4", MEM32(drv));
    ok("a sample handle", s != 0);
    CALL(sfcop_hle_mss, "_AIL_init_sample@4", s);
    ok("a new sample is done", CALL(sfcop_hle_mss, "_AIL_sample_status@4", s) == 2);
    CALL(sfcop_hle_mss, "_AIL_set_sample_type@12", s, 1, 0);           /* DIG_F_MONO_16 */
    CALL(sfcop_hle_mss, "_AIL_set_sample_playback_rate@8", s, 22050);
    CALL(sfcop_hle_mss, "_AIL_set_sample_volume@8", s, 100);
    ok("volume reads back", CALL(sfcop_hle_mss, "_AIL_sample_volume@4", s) == 100);
    CALL(sfcop_hle_mss, "_AIL_set_sample_user_data@12", s, 3, 0xBEEF);
    ok("buffer 0 is the one to fill", CALL(sfcop_hle_mss, "_AIL_sample_buffer_ready@4", s) == 0);
    uint32_t data = gbuf(200);                           /* 100 frames of 16-bit mono */
    CALL(sfcop_hle_mss, "_AIL_load_sample_buffer@16", s, 0, data, 200);
    ok("loading starts the sample", CALL(sfcop_hle_mss, "_AIL_sample_status@4", s) == 4);
    ok("then buffer 1", CALL(sfcop_hle_mss, "_AIL_sample_buffer_ready@4", s) == 1);
    CALL(sfcop_hle_mss, "_AIL_load_sample_buffer@16", s, 1, data, 200);
    ok("both full", CALL(sfcop_hle_mss, "_AIL_sample_buffer_ready@4", s) == 0xFFFFFFFFu);
    sfcop_mss_advance(300);                              /* 150 source frames at 22050 Hz */
    ok("buffer 0 played out: fill it", CALL(sfcop_hle_mss, "_AIL_sample_buffer_ready@4", s) == 0);
    ok("still playing", CALL(sfcop_hle_mss, "_AIL_sample_status@4", s) == 4);
    sfcop_mss_advance(200);                              /* 100 more: past the end of buffer 1 */
    ok("starved: done", CALL(sfcop_hle_mss, "_AIL_sample_status@4", s) == 2);
    CALL(sfcop_hle_mss, "_AIL_load_sample_buffer@16", s, 0, data, 200);
    ok("a load restarts it", CALL(sfcop_hle_mss, "_AIL_sample_status@4", s) == 4);
    CALL(sfcop_hle_mss, "_AIL_end_sample@4", s);
    ok("ended", CALL(sfcop_hle_mss, "_AIL_sample_status@4", s) == 2);
    ok("minimum buffer for 22 kHz 16-bit mono: 4 KB", CALL(sfcop_hle_mss, "_AIL_minimum_sample_buffer_size@12", MEM32(drv), 22050, 1) == 4096);
    uint32_t old = CALL(sfcop_hle_mss, "_AIL_set_redist_directory@4", gstr("redist"));
    ok("set_redist_directory: the previous one, in guest memory", old && MEM8(old) == 0);
    old = CALL(sfcop_hle_mss, "_AIL_set_redist_directory@4", gstr("other"));
    ok("set_redist_directory remembers", !strcmp((const char *)hle_ptr(old), "redist"));
    CALL(sfcop_hle_mss, "_AIL_release_sample_handle@4", s);
    ok("a released handle reads as free", CALL(sfcop_hle_mss, "_AIL_sample_status@4", s) == 1);
    CALL0(sfcop_hle_mss, "_AIL_serve@0");
    CALL(sfcop_hle_mss, "_AIL_waveOutClose@4", MEM32(drv));
}

static void test_ds3d(void) {
    /* any IUnknown-shaped buffer will do: the override answers the 3D IIDs itself */
    uint32_t buf = gbuf(16), iid = gbuf(16), out = gbuf(4), v = gbuf(12);
    memcpy(hle_ptr(iid), IID_3DBuffer, 16);
    ok("QueryInterface(3D buffer)", CALL(sfcop_hle_ds3d, "IDirectSoundBuffer::QueryInterface", buf, iid, out) == 0);
    uint32_t b3 = MEM32(out);
    ok("SetPosition(1, 2, 3)", VCALL(b3, 19, f32(1.0f), f32(2.0f), f32(3.0f), 0) == 0);
    ok("GetPosition", VCALL(b3, 10, v) == 0 && MEM32(v) == f32(1.0f) && MEM32(v + 4) == f32(2.0f) && MEM32(v + 8) == f32(3.0f));
    ok("SetConeOutsideVolume(-600)", VCALL(b3, 15, (uint32_t)-600, 0) == 0);
    ok("GetConeOutsideVolume", VCALL(b3, 6, v) == 0 && MEM32(v) == (uint32_t)-600);
    ok("default max distance 1e9", VCALL(b3, 7, v) == 0 && MEM32(v) == f32(1000000000.0f));
    ok("Release the 3D buffer", VCALL0(b3, 2) == 0);
    memcpy(hle_ptr(iid), IID_3DListener, 16);
    ok("QueryInterface(listener)", CALL(sfcop_hle_ds3d, "IDirectSoundBuffer::QueryInterface", buf, iid, out) == 0);
    uint32_t l = MEM32(out), front = gbuf(12), top = gbuf(12);
    ok("SetOrientation", VCALL(l, 13, f32(0), f32(0), f32(1), f32(0), f32(1), f32(0), 0) == 0);
    ok("GetOrientation", VCALL(l, 6, front, top) == 0 && MEM32(front + 8) == f32(1) && MEM32(top + 4) == f32(1));
    ok("SetDistanceFactor(2)", VCALL(l, 11, f32(2.0f), 0) == 0);
    ok("GetDistanceFactor", VCALL(l, 4, v) == 0 && MEM32(v) == f32(2.0f));
    ok("CommitDeferredSettings", VCALL0(l, 17) == 0);
}

static void test_wide(const char *dir) {
    hle_set_drive('C', dir);
    uint32_t h = CALL(sfcop_hle_widefile, "CreateFileW", gwstr("C:\\wide.txt"), 0xC0000000u, 0, 0, 2, 0x80, 0);
    ok("CreateFileW makes a file", h != 0xFFFFFFFFu);
    CALL(NULL, "CloseHandle", h);
    uint32_t fd = gbuf(592);
    h = CALL(sfcop_hle_widefile, "FindFirstFileW", gwstr("C:\\*.txt"), fd);
    ok("FindFirstFileW finds it", h != 0xFFFFFFFFu && MEM16(fd + 44) == 'w' && MEM16(fd + 44 + 2 * 8) == 0
       && !memcmp(hle_ptr(fd + 44), "w\0i\0d\0e\0.\0t\0x\0t\0", 16));
    ok("FindNextFileW: no more", CALL(sfcop_hle_widefile, "FindNextFileW", h, fd) == 0);
    ok("GetFileAttributesW", CALL(sfcop_hle_widefile, "GetFileAttributesW", gwstr("C:\\wide.txt")) != 0xFFFFFFFFu);
    ok("CreateDirectoryW", CALL(sfcop_hle_widefile, "CreateDirectoryW", gwstr("C:\\sub"), 0) == 1);
    ok("RemoveDirectoryW", CALL(sfcop_hle_widefile, "RemoveDirectoryW", gwstr("C:\\sub")) == 1);
    ok("DeleteFileW", CALL(sfcop_hle_widefile, "DeleteFileW", gwstr("C:\\wide.txt")) == 1);
    ok("the file is gone", CALL(sfcop_hle_widefile, "GetFileAttributesW", gwstr("C:\\wide.txt")) == 0xFFFFFFFFu);
    uint32_t wb = gbuf(2 * 300);
    ok("SetCurrentDirectoryW", CALL(sfcop_hle_widefile, "SetCurrentDirectoryW", gwstr("C:\\")) == 1);
    uint32_t n = CALL(sfcop_hle_widefile, "GetCurrentDirectoryW", 300, wb);
    ok("GetCurrentDirectoryW: C:\\ in characters", n == 3 && MEM16(wb) == 'C' && MEM16(wb + 2) == ':' && MEM16(wb + 4) == '\\' && MEM16(wb + 6) == 0);
    ok("GetCurrentDirectoryW: too small asks for 4", CALL(sfcop_hle_widefile, "GetCurrentDirectoryW", 2, wb) == 4);
}

static void test_misc(void) {
    ok("VkKeyScanA('a') is 'A'", CALL(sfcop_hle_misc, "VkKeyScanA", 'a') == 0x41);
    ok("VkKeyScanA('A') is Shift+'A'", CALL(sfcop_hle_misc, "VkKeyScanA", 'A') == 0x141);
    ok("VkKeyScanA('!') is Shift+'1'", CALL(sfcop_hle_misc, "VkKeyScanA", '!') == 0x131);
    ok("VkKeyScanA('/') is VK_OEM_2", CALL(sfcop_hle_misc, "VkKeyScanA", '/') == 0xBF);
    uint32_t sp = gbuf(64);
    ok("GetShortPathNameA copies", CALL(sfcop_hle_misc, "GetShortPathNameA", gstr("C:\\Long Name"), sp, 64) == 12
       && !strcmp((const char *)hle_ptr(sp), "C:\\Long Name"));
    ok("GetShortPathNameA: too small asks for 13", CALL(sfcop_hle_misc, "GetShortPathNameA", gstr("C:\\Long Name"), sp, 4) == 13);
    uint32_t dm = gbuf(156);
    MEM16(dm + 36) = 156;
    ok("EnumDisplaySettingsA(0): a mode", CALL(sfcop_hle_misc, "EnumDisplaySettingsA", 0, 0, dm) == 1 && MEM32(dm + 108) && MEM32(dm + 104) == 8);
    ok("EnumDisplaySettingsA past the end", CALL(sfcop_hle_misc, "EnumDisplaySettingsA", 0, 10000, dm) == 0);
    uint32_t rc = gbuf(16);
    ok("SystemParametersInfoA(SPI_GETWORKAREA)", CALL(sfcop_hle_misc, "SystemParametersInfoA", 0x30, 0, rc, 0) == 1 && MEM32(rc + 8) > 0);
    ok("WinExec runs nothing", CALL(sfcop_hle_misc, "WinExec", gstr("notepad"), 1) < 32);
    uint32_t out = gbuf(4), hw = gbuf(4);
    ok("DirectDrawCreateClipper", CALL(sfcop_hle_misc, "DirectDrawCreateClipper", 0, out, 0) == 0);
    ok("clipper SetHWnd", VCALL(MEM32(out), 8, 0, 0x5678) == 0);
    ok("clipper GetHWnd", VCALL(MEM32(out), 4, hw) == 0 && MEM32(hw) == 0x5678);
    ok("SymInitialize", CALL(sfcop_hle_misc, "SymInitialize", 1, 0, 1) == 1);
    ok("StackWalk finds nothing", CALL(sfcop_hle_misc, "StackWalk", 0x14C, 1, 2, 0, 0, 0, 0, 0, 0) == 0);
    ok("GetOpenFileNameA: cancelled", CALL(sfcop_hle_misc, "GetOpenFileNameA", gbuf(88)) == 0);
    ok("CommDlgExtendedError: none", CALL0(sfcop_hle_misc, "CommDlgExtendedError") == 0);
}

/* patches/pcrecomp-kernel32-unnamed-objects.patch: "" is no name */
static void test_unnamed_events(void) {
    uint32_t empty = gstr(""), name = gstr("sfcop-test");
    uint32_t a = CALL(NULL, "CreateEventA", 0, 0, 0, empty);
    uint32_t b = CALL(NULL, "CreateEventA", 0, 0, 0, empty);
    ok("two events named \"\" are two events", a && b && a != b);
    uint32_t c = CALL(NULL, "CreateEventA", 0, 0, 0, name);
    uint32_t d = CALL(NULL, "CreateEventA", 0, 0, 0, name);
    ok("two events of one name are one", c && c == d);
    CALL(NULL, "SetEvent", a);
    ok("setting one leaves the other", CALL(NULL, "WaitForSingleObject", b, 0) == 0x102u);
    ok("the one set is signalled", CALL(NULL, "WaitForSingleObject", a, 0) == 0);
}

/* patches/pcrecomp-wsock32-select-accept.patch, over loopback */
static void test_winsock(void) {
    const win32hle_shim *w = NULL;
    uint32_t wsa = gbuf(400), addr = gbuf(16), len = gbuf(4), set = gbuf(4 + 4 * 64), tv = gbuf(8);
    ok("WSAStartup", CALL(w, "WSAStartup", 0x101, wsa) == 0);
    uint32_t ls = CALL(w, "socket", 2, 1, 0);
    MEM16(addr) = 2, MEM16(addr + 2) = 0, MEM32(addr + 4) = 0x0100007Fu;   /* 127.0.0.1, any port */
    ok("bind", CALL(w, "bind", ls, addr, 16) == 0);
    ok("listen", CALL(w, "listen", ls, 4) == 0);
    MEM32(len) = 16;
    CALL(w, "getsockname", ls, addr, len);
    MEM32(set) = 1, MEM32(set + 4) = ls, MEM32(tv) = 0, MEM32(tv + 4) = 0;
    ok("select: nothing to accept yet", CALL(w, "select", 0, set, 0, 0, tv) == 0 && MEM32(set) == 0);
    uint32_t cs = CALL(w, "socket", 2, 1, 0);
    ok("connect", CALL(w, "connect", cs, addr, 16) == 0);
    MEM32(set) = 1, MEM32(set + 4) = ls, MEM32(tv) = 1, MEM32(tv + 4) = 0;
    ok("select: the listener is readable", CALL(w, "select", 0, set, 0, 0, tv) == 1);
    ok("__WSAFDIsSet: it is in the set", CALL(w, "__WSAFDIsSet", ls, set) == 1);
    ok("__WSAFDIsSet: another is not", CALL(w, "__WSAFDIsSet", cs, set) == 0);
    MEM32(len) = 16;
    uint32_t as = CALL(w, "accept", ls, addr, len);
    ok("accept", as != 0xFFFFFFFFu && MEM32(len) == 16 && MEM16(addr) == 2 && MEM32(addr + 4) == 0x0100007Fu);
    uint32_t peer = gbuf(16);
    MEM32(len) = 16;
    ok("getpeername: loopback", CALL(w, "getpeername", as, peer, len) == 0 && MEM32(peer + 4) == 0x0100007Fu);
    MEM32(set) = 1, MEM32(set + 4) = cs, MEM32(tv) = 0, MEM32(tv + 4) = 0;
    ok("select: the client is writable", CALL(w, "select", 0, 0, set, 0, tv) == 1);
    ok("shutdown(SD_SEND)", CALL(w, "shutdown", cs, 1) == 0);
    MEM32(set) = 1, MEM32(set + 4) = as, MEM32(tv) = 1, MEM32(tv + 4) = 0;
    ok("select: the peer sees the end", CALL(w, "select", 0, set, 0, 0, tv) == 1);
    CALL(w, "closesocket", as), CALL(w, "closesocket", cs), CALL(w, "closesocket", ls);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : NULL;
    if (!dir) { fprintf(stderr, "usage: hle_selftest SCRATCH_DIR\n"); return 64; }
    mkdir(dir, 0755);
    char full[1024];
    if (!realpath(dir, full)) { fprintf(stderr, "no %s\n", dir); return 1; }
    win32hle_register(win32hle_kernel32);
    win32hle_register(win32hle_kernel32_ext);
    win32hle_register(win32hle_kernel32_crt);
    win32hle_register(win32hle_module);
    win32hle_register(win32hle_wsock32);
    mach_enter();
    test_dinput();
    test_mss();
    test_ds3d();
    test_wide(full);
    test_misc();
    test_unnamed_events();
    test_winsock();
    mach_leave();
    printf("hle_selftest: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
