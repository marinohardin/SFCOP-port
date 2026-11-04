// TEMPORARY: replaced by Wave 1 B2 (DirectDraw clipper) and B5 (kernel, user, GDI and dialog calls)
/*
 * misc_stubs.c - the small Win32 gaps StarFleetOP.exe imports and win32hle
 * does not answer: KERNEL32 odds and ends, USER32 display and keyboard
 * queries, two GDI32 calls, COMDLG32 file dialogs, the ADVAPI32 event log,
 * DBGHELP (a crash reporter's stack walk) and DirectDrawCreateClipper.
 * Upstreamable: yes. Each answers as Windows does when the feature is
 * absent or does nothing visible; the ones that matter later are listed in
 * README.md ("Stubs").
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "sfcop_hle.h"

#define ARG_SLOT(i) (g_esp + 4u + 4u * (uint32_t)(i))

static void run_original(const char *name) {
    recomp_func_t f = sfcop_hle_original(name);
    if (!f) hle_fatal("misc_stubs: win32hle has no %s", name);
    f();
}

/* ---- KERNEL32 ---- */
static void k_WinExec(void) {                            /* (cmdline, show): < 32 is a failure */
    fprintf(stderr, "[kernel32] WinExec(\"%s\"): no programs are run\n", ASTR(0) ? ASTR(0) : "");
    RET(2u, 2);                                          /* ERROR_FILE_NOT_FOUND */
}
static void k_OpenFileMappingA(void) { hle_set_last_error(2u); RET(0, 3); }
static void k_VirtualQueryEx(void) { sfcop_todo_once("VirtualQueryEx", "answers 0 bytes"); RET(0, 4); }
static void k_GetThreadContext(void) { RET(0, 2); }
static void k_GetShortPathNameA(void) {                  /* (long, short, n): the long name serves */
    const char *s = ASTR(0);
    uint32_t n = A32(2), len = s ? (uint32_t)strlen(s) : 0;
    if (!s) RET(0, 3);
    if (len + 1 > n || !A32(1)) RET(len + 1, 3);
    if (A32(1) != A32(0)) memmove(APTR(1), s, len + 1);
    RET(len, 3);
}
static void k_VirtualLock(void) { RET(1, 2); }
static void k_MapViewOfFileEx(void) {                    /* MapViewOfFile's five and a wished-for base */
    run_original("MapViewOfFile");                       /* pops ret + 5 */
    g_esp += 4;                                          /* and the base */
}

/* ---- USER32 ---- */
static void u_GetSystemMenu(void) { RET(0, 2); }
static void u_ChangeDisplaySettingsA(void) { RET(0, 2); }   /* DISP_CHANGE_SUCCESSFUL: DirectDraw sets the mode */
static void u_IsIconic(void) { RET(0, 1); }
/* (device, mode, DEVMODEA*): the modes DirectDraw offers, at 8, 16 and 32 bits */
static void u_EnumDisplaySettingsA(void) {
    uint32_t mode = A32(1), dm = A32(2);
    int w = 640, h = 480, bpp = 16, dw = 0, dh = 0;
    if (!dm) RET(0, 3);
    if (mode == 0xFFFFFFFFu || mode == 0xFFFFFFFEu) {    /* ENUM_CURRENT_SETTINGS, ENUM_REGISTRY_SETTINGS */
        hle_dd_mode(&w, &h, &bpp);
        if (w <= 0 || h <= 0) { if (!hle_screen_desktop(&w, &h)) w = 1024, h = 768; bpp = 32; }
    } else {
        int modes[HLE_DD_MAX_MODES][2];
        hle_screen_desktop(&dw, &dh);
        int n = hle_dd_modes(dw, dh, modes);
        static const int depths[3] = { 8, 16, 32 };
        if (mode >= (uint32_t)n * 3u) RET(0, 3);
        w = modes[mode / 3][0], h = modes[mode / 3][1], bpp = depths[mode % 3];
    }
    uint16_t size = MEM16(dm + 36);
    memset(hle_ptr(dm), 0, size >= 124 ? size : 124);
    strcpy((char *)hle_ptr(dm), "Display");
    MEM16(dm + 36) = size >= 124 ? size : 124;
    MEM32(dm + 40) = 0x00040000u | 0x00080000u | 0x00100000u | 0x00400000u;   /* BITSPERPEL, PELSWIDTH, PELSHEIGHT, DISPLAYFREQUENCY */
    MEM32(dm + 104) = (uint32_t)bpp, MEM32(dm + 108) = (uint32_t)w, MEM32(dm + 112) = (uint32_t)h;
    MEM32(dm + 120) = 60;
    RET(1, 3);
}
/* (char) -> VK in the low byte, shift state (1 = Shift) in the high byte */
static void u_VkKeyScanA(void) {
    int c = (int)(A32(0) & 0xFF);
    static const char shifted[] = ")!@#$%^&*(";
    static const struct { char c; uint8_t vk, shift; } punct[] = {
        { ';', 0xBA, 0 }, { ':', 0xBA, 1 }, { '=', 0xBB, 0 }, { '+', 0xBB, 1 }, { ',', 0xBC, 0 }, { '<', 0xBC, 1 },
        { '-', 0xBD, 0 }, { '_', 0xBD, 1 }, { '.', 0xBE, 0 }, { '>', 0xBE, 1 }, { '/', 0xBF, 0 }, { '?', 0xBF, 1 },
        { '`', 0xC0, 0 }, { '~', 0xC0, 1 }, { '[', 0xDB, 0 }, { '{', 0xDB, 1 }, { '\\', 0xDC, 0 }, { '|', 0xDC, 1 },
        { ']', 0xDD, 0 }, { '}', 0xDD, 1 }, { '\'', 0xDE, 0 }, { '"', 0xDE, 1 }, { ' ', 0x20, 0 }, { '\r', 0x0D, 0 },
        { '\t', 0x09, 0 }, { '\b', 0x08, 0 }, { 27, 0x1B, 0 } };
    if (c >= 'a' && c <= 'z') RET((uint32_t)toupper(c), 1);
    if (c >= 'A' && c <= 'Z') RET(0x100u | (uint32_t)c, 1);
    if (c >= '0' && c <= '9') RET((uint32_t)c, 1);
    for (int i = 0; shifted[i]; i++) if (shifted[i] == c) RET(0x100u | (uint32_t)('0' + i), 1);
    for (size_t i = 0; i < sizeof punct / sizeof punct[0]; i++)
        if (punct[i].c == c) RET((uint32_t)punct[i].shift << 8 | punct[i].vk, 1);
    RET(0xFFFFu, 1);
}
/* (action, uiParam, pvParam, winini) */
static void u_SystemParametersInfoA(void) {
    uint32_t action = A32(0), pv = A32(2);
    switch (action) {
    case 0x0030: {                                       /* SPI_GETWORKAREA: the whole screen */
        int w = 0, h = 0, bpp;
        hle_dd_mode(&w, &h, &bpp);
        if (w <= 0 || h <= 0) { if (!hle_screen_desktop(&w, &h)) w = 1024, h = 768; }
        if (pv) MEM32(pv) = 0, MEM32(pv + 4) = 0, MEM32(pv + 8) = (uint32_t)w, MEM32(pv + 12) = (uint32_t)h;
        break;
    }
    case 0x0010: case 0x0032: case 0x0034: case 0x003A:   /* screen saver active; FILTER-, TOGGLE-, STICKYKEYS: off */
        if (pv) MEM32(pv + (action == 0x0010 ? 0 : 4)) = 0;
        break;
    default:
        break;                                           /* the SET actions, and the rest: accepted */
    }
    RET(1, 4);
}

/* ---- GDI32 ---- */
/* ExtTextOutA(hdc, x, y, options, rect, str, count, dx) as TextOutA(hdc, x, y,
 * str, count): the clip/opaque rectangle and the spacing are not applied. */
static void g_ExtTextOutA(void) {
    if (A32(3) & 2u) sfcop_todo_once("ExtTextOutA", "ETO_OPAQUE rectangle not filled");
    uint32_t str = A32(5), count = A32(6);
    MEM32(ARG_SLOT(3)) = str, MEM32(ARG_SLOT(4)) = count;
    run_original("TextOutA");                            /* pops ret + 5 */
    g_esp += 12;                                         /* str, count, dx */
}
static void g_GetDIBColorTable(void) { RET(0, 4); }

/* ---- COMDLG32: no file dialogs; the caller sees a cancel ---- */
static void c_GetFileName(void) {
    sfcop_todo_once("GetOpenFileName/GetSaveFileName", "answered as cancelled");
    RET(0, 1);
}
static void c_CommDlgExtendedError(void) { RET(0, 0); }

/* ---- ADVAPI32 event log: to stderr ---- */
static void a_RegisterEventSourceA(void) { RET(0x4E00u, 2); }
static void a_DeregisterEventSource(void) { RET(1, 1); }
static void a_ReportEventA(void) {                       /* (h, type, category, id, sid, nstrings, datasize, strings, data) */
    uint32_t n = A32(5) & 0xFFFF, strs = A32(7);
    fprintf(stderr, "[eventlog] type %u id 0x%08X:", A32(1) & 0xFFFF, A32(3));
    for (uint32_t i = 0; strs && i < n; i++) {
        uint32_t s = MEM32(strs + 4 * i);
        fprintf(stderr, " %s", s ? (const char *)hle_ptr(s) : "(null)");
    }
    fputc('\n', stderr);
    RET(1, 9);
}

/* ---- DBGHELP: no symbols, no stack walk ---- */
static void d_SymInitialize(void) { RET(1, 3); }
static void d_SymGetOptions(void) { RET(0, 0); }
static void d_SymSetOptions(void) { RET(A32(0), 1); }
static void d_SymLoadModule(void) { RET(0, 6); }
static void d_SymGetSymFromAddr(void) { RET(0, 4); }
static void d_SymGetModuleInfo(void) { RET(0, 3); }
static void d_SymGetLineFromAddr(void) { RET(0, 4); }
static void d_SymFunctionTableAccess(void) { RET(0, 2); }
static void d_StackWalk(void) { RET(0, 9); }

/* ---- DDRAW: a clipper without a DirectDraw object ----
 * Laid out as win32hle's own clippers (vtable, refs, HWND at +8), so a
 * surface's SetClipper takes it the same way. */
static void cl_Release(void) { uint32_t self = A32(0), n = --MEM32(self + 4); if (!n) hle_com_free(self); RET(n, 1); }
static void cl_GetClipList(void) { if (A32(3)) MEM32(A32(3)) = 0; RET(0x80004001u, 4); }   /* DDERR_UNSUPPORTED */
static void cl_GetHWnd(void) { if (A32(1)) MEM32(A32(1)) = MEM32(A32(0) + 8); RET(0, 2); }
static void cl_Initialize(void) { RET(0, 3); }
static void cl_IsClipListChanged(void) { if (A32(1)) MEM32(A32(1)) = 0; RET(0, 2); }
static void cl_SetClipList(void) { RET(0, 3); }
static void cl_SetHWnd(void) { MEM32(A32(0) + 8) = A32(2); RET(0, 3); }
static void dd_DirectDrawCreateClipper(void) {           /* (flags, &out, outer) */
    static uint32_t vt;
    if (!vt) {
        static const win32hle_shim cl[] = {
            { "DirectDrawClipper::QueryInterface", hle_com_QueryInterface }, { "DirectDrawClipper::AddRef", hle_com_AddRef },
            { "DirectDrawClipper::Release", cl_Release }, { "DirectDrawClipper::GetClipList", cl_GetClipList },
            { "DirectDrawClipper::GetHWnd", cl_GetHWnd }, { "DirectDrawClipper::Initialize", cl_Initialize },
            { "DirectDrawClipper::IsClipListChanged", cl_IsClipListChanged },
            { "DirectDrawClipper::SetClipList", cl_SetClipList }, { "DirectDrawClipper::SetHWnd", cl_SetHWnd }, { 0, 0 } };
        vt = hle_com_vtable(cl);
    }
    if (!A32(1)) RET(0x80070057u, 3);
    MEM32(A32(1)) = hle_com_new(vt, 12);
    RET(0, 3);
}

const win32hle_shim sfcop_hle_misc[] = {
    { "WinExec", k_WinExec }, { "OpenFileMappingA", k_OpenFileMappingA }, { "VirtualQueryEx", k_VirtualQueryEx },
    { "GetThreadContext", k_GetThreadContext }, { "GetShortPathNameA", k_GetShortPathNameA },
    { "VirtualLock", k_VirtualLock }, { "MapViewOfFileEx", k_MapViewOfFileEx },
    { "GetSystemMenu", u_GetSystemMenu }, { "ChangeDisplaySettingsA", u_ChangeDisplaySettingsA },
    { "EnumDisplaySettingsA", u_EnumDisplaySettingsA }, { "VkKeyScanA", u_VkKeyScanA }, { "IsIconic", u_IsIconic },
    { "SystemParametersInfoA", u_SystemParametersInfoA },
    { "ExtTextOutA", g_ExtTextOutA }, { "GetDIBColorTable", g_GetDIBColorTable },
    { "GetOpenFileNameA", c_GetFileName }, { "GetOpenFileNameW", c_GetFileName },
    { "GetSaveFileNameA", c_GetFileName }, { "GetSaveFileNameW", c_GetFileName },
    { "CommDlgExtendedError", c_CommDlgExtendedError },
    { "RegisterEventSourceA", a_RegisterEventSourceA }, { "DeregisterEventSource", a_DeregisterEventSource },
    { "ReportEventA", a_ReportEventA },
    { "SymInitialize", d_SymInitialize }, { "SymGetOptions", d_SymGetOptions }, { "SymSetOptions", d_SymSetOptions },
    { "SymLoadModule", d_SymLoadModule }, { "SymGetSymFromAddr", d_SymGetSymFromAddr },
    { "SymGetModuleInfo", d_SymGetModuleInfo }, { "SymGetLineFromAddr", d_SymGetLineFromAddr },
    { "SymFunctionTableAccess", d_SymFunctionTableAccess }, { "StackWalk", d_StackWalk },
    { "DirectDrawCreateClipper", dd_DirectDrawCreateClipper },
    { 0, 0 }
};
