// TEMPORARY: replaced by Wave 1 B3 (input)
/*
 * dinput.c - DirectInput (DINPUT.DLL, interface versions 3 to 7) for the
 * system keyboard, on SDL2's keyboard state. Upstreamable: yes.
 *
 * DirectInputCreateA hands out an IDirectInputA whose CreateDevice serves
 * GUID_SysKeyboard; the device answers both immediate reads (GetDeviceState:
 * 256 bytes indexed by DIK_ code, 0x80 when down) and buffered reads
 * (GetDeviceData, once SetProperty(DIPROP_BUFFERSIZE) gave it a buffer).
 * DIK_ codes are the PC set-1 scan codes, extended keys with bit 7 set.
 *
 * The keyboard state is SDL's, which the presenter's event pump keeps
 * current (screen.c pumps from PeekMessage/GetMessage). Buffered events are
 * made by comparing that state with the last one reported, so a press and a
 * release between two reads of the buffer are not seen; a game that polls
 * once a frame does not notice. The mouse and joysticks are not served
 * (CreateDevice answers DIERR_DEVICENOTREG): StarFleetOP.exe asks only for the
 * keyboard and takes the mouse from window messages.
 */
#include <stdio.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "sfcop_hle.h"

#define DI_OK                 0u
#define DI_NOEFFECT           1u            /* S_FALSE */
#define DI_BUFFEROVERFLOW     1u
#define DIERR_INVALIDPARAM    0x80070057u
#define DIERR_NOTACQUIRED     0x8007000Cu
#define DIERR_DEVICENOTREG    0x80040154u
#define DIERR_UNSUPPORTED     0x80004001u
#define DIERR_NOINTERFACE     0x80004002u
#define DIERR_OBJECTNOTFOUND  0x80070002u
#define DIDEVTYPE_KEYBOARD    3u
#define DIDEVTYPEKEYBOARD_PCENH 4u
#define DIGDD_PEEK            1u
#define DIPROP_BUFFERSIZE     1u            /* MAKEDIPROP(1): the GUID pointer is the number */
#define DIENUM_STOP           0u

static const uint8_t GUID_SysKeyboard[16] = { 0x61, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11,
                                              0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };

/* SDL scancode -> DIK_ code. */
static uint8_t dik_of(SDL_Scancode s) {
    static const uint8_t t[SDL_NUM_SCANCODES] = {
        [SDL_SCANCODE_ESCAPE] = 0x01, [SDL_SCANCODE_1] = 0x02, [SDL_SCANCODE_2] = 0x03, [SDL_SCANCODE_3] = 0x04,
        [SDL_SCANCODE_4] = 0x05, [SDL_SCANCODE_5] = 0x06, [SDL_SCANCODE_6] = 0x07, [SDL_SCANCODE_7] = 0x08,
        [SDL_SCANCODE_8] = 0x09, [SDL_SCANCODE_9] = 0x0A, [SDL_SCANCODE_0] = 0x0B, [SDL_SCANCODE_MINUS] = 0x0C,
        [SDL_SCANCODE_EQUALS] = 0x0D, [SDL_SCANCODE_BACKSPACE] = 0x0E, [SDL_SCANCODE_TAB] = 0x0F,
        [SDL_SCANCODE_Q] = 0x10, [SDL_SCANCODE_W] = 0x11, [SDL_SCANCODE_E] = 0x12, [SDL_SCANCODE_R] = 0x13,
        [SDL_SCANCODE_T] = 0x14, [SDL_SCANCODE_Y] = 0x15, [SDL_SCANCODE_U] = 0x16, [SDL_SCANCODE_I] = 0x17,
        [SDL_SCANCODE_O] = 0x18, [SDL_SCANCODE_P] = 0x19, [SDL_SCANCODE_LEFTBRACKET] = 0x1A,
        [SDL_SCANCODE_RIGHTBRACKET] = 0x1B, [SDL_SCANCODE_RETURN] = 0x1C, [SDL_SCANCODE_LCTRL] = 0x1D,
        [SDL_SCANCODE_A] = 0x1E, [SDL_SCANCODE_S] = 0x1F, [SDL_SCANCODE_D] = 0x20, [SDL_SCANCODE_F] = 0x21,
        [SDL_SCANCODE_G] = 0x22, [SDL_SCANCODE_H] = 0x23, [SDL_SCANCODE_J] = 0x24, [SDL_SCANCODE_K] = 0x25,
        [SDL_SCANCODE_L] = 0x26, [SDL_SCANCODE_SEMICOLON] = 0x27, [SDL_SCANCODE_APOSTROPHE] = 0x28,
        [SDL_SCANCODE_GRAVE] = 0x29, [SDL_SCANCODE_LSHIFT] = 0x2A, [SDL_SCANCODE_BACKSLASH] = 0x2B,
        [SDL_SCANCODE_Z] = 0x2C, [SDL_SCANCODE_X] = 0x2D, [SDL_SCANCODE_C] = 0x2E, [SDL_SCANCODE_V] = 0x2F,
        [SDL_SCANCODE_B] = 0x30, [SDL_SCANCODE_N] = 0x31, [SDL_SCANCODE_M] = 0x32, [SDL_SCANCODE_COMMA] = 0x33,
        [SDL_SCANCODE_PERIOD] = 0x34, [SDL_SCANCODE_SLASH] = 0x35, [SDL_SCANCODE_RSHIFT] = 0x36,
        [SDL_SCANCODE_KP_MULTIPLY] = 0x37, [SDL_SCANCODE_LALT] = 0x38, [SDL_SCANCODE_SPACE] = 0x39,
        [SDL_SCANCODE_CAPSLOCK] = 0x3A, [SDL_SCANCODE_F1] = 0x3B, [SDL_SCANCODE_F2] = 0x3C, [SDL_SCANCODE_F3] = 0x3D,
        [SDL_SCANCODE_F4] = 0x3E, [SDL_SCANCODE_F5] = 0x3F, [SDL_SCANCODE_F6] = 0x40, [SDL_SCANCODE_F7] = 0x41,
        [SDL_SCANCODE_F8] = 0x42, [SDL_SCANCODE_F9] = 0x43, [SDL_SCANCODE_F10] = 0x44,
        [SDL_SCANCODE_NUMLOCKCLEAR] = 0x45, [SDL_SCANCODE_SCROLLLOCK] = 0x46, [SDL_SCANCODE_KP_7] = 0x47,
        [SDL_SCANCODE_KP_8] = 0x48, [SDL_SCANCODE_KP_9] = 0x49, [SDL_SCANCODE_KP_MINUS] = 0x4A,
        [SDL_SCANCODE_KP_4] = 0x4B, [SDL_SCANCODE_KP_5] = 0x4C, [SDL_SCANCODE_KP_6] = 0x4D,
        [SDL_SCANCODE_KP_PLUS] = 0x4E, [SDL_SCANCODE_KP_1] = 0x4F, [SDL_SCANCODE_KP_2] = 0x50,
        [SDL_SCANCODE_KP_3] = 0x51, [SDL_SCANCODE_KP_0] = 0x52, [SDL_SCANCODE_KP_PERIOD] = 0x53,
        [SDL_SCANCODE_NONUSBACKSLASH] = 0x56, [SDL_SCANCODE_F11] = 0x57, [SDL_SCANCODE_F12] = 0x58,
        [SDL_SCANCODE_KP_ENTER] = 0x9C, [SDL_SCANCODE_RCTRL] = 0x9D, [SDL_SCANCODE_KP_DIVIDE] = 0xB5,
        [SDL_SCANCODE_PRINTSCREEN] = 0xB7, [SDL_SCANCODE_RALT] = 0xB8, [SDL_SCANCODE_PAUSE] = 0xC5,
        [SDL_SCANCODE_HOME] = 0xC7, [SDL_SCANCODE_UP] = 0xC8, [SDL_SCANCODE_PAGEUP] = 0xC9,
        [SDL_SCANCODE_LEFT] = 0xCB, [SDL_SCANCODE_RIGHT] = 0xCD, [SDL_SCANCODE_END] = 0xCF,
        [SDL_SCANCODE_DOWN] = 0xD0, [SDL_SCANCODE_PAGEDOWN] = 0xD1, [SDL_SCANCODE_INSERT] = 0xD2,
        [SDL_SCANCODE_DELETE] = 0xD3, [SDL_SCANCODE_LGUI] = 0xDB, [SDL_SCANCODE_RGUI] = 0xDC,
        [SDL_SCANCODE_APPLICATION] = 0xDD };
    return (unsigned)s < SDL_NUM_SCANCODES ? t[s] : 0;
}

/* The keyboard now, by DIK_ code. */
static void snapshot(uint8_t out[256]) {
    int n = 0;
    const Uint8 *k = SDL_GetKeyboardState(&n);
    memset(out, 0, 256);
    for (int s = 0; k && s < n; s++)
        if (k[s]) { uint8_t d = dik_of((SDL_Scancode)s); if (d) out[d] = 0x80; }
}

/* ---- the device ---- */
#define MAX_DEVS 4
#define RING 256
typedef struct {
    int used, acquired, buffer_size, overflow;
    uint32_t data_size;                 /* SetDataFormat's dwDataSize (256 for c_dfDIKeyboard) */
    uint8_t last[256];                  /* the state the buffer has reported */
    struct { uint32_t ofs, data, time, seq; } ev[RING];
    uint32_t head, count, seq;
} kbd_t;
static kbd_t g_dev[MAX_DEVS];
static uint32_t g_vt_di, g_vt_dev;
#define DEV(self) (MEM32((self) + 8) < MAX_DEVS ? &g_dev[MEM32((self) + 8)] : NULL)

/* Queue an event for each key whose state changed since the last report. */
static void collect(kbd_t *d) {
    uint8_t now[256];
    snapshot(now);
    for (int i = 0; i < 256; i++) {
        if (now[i] == d->last[i]) continue;
        d->last[i] = now[i];
        if (!d->buffer_size) continue;
        if (d->count == (uint32_t)d->buffer_size || d->count == RING) { d->overflow = 1; continue; }
        uint32_t k = (d->head + d->count++) % RING;
        d->ev[k].ofs = (uint32_t)i, d->ev[k].data = now[i], d->ev[k].time = hle_ticks_ms(), d->ev[k].seq = ++d->seq;
    }
}

static void dv_Release(void) {
    uint32_t self = A32(0), n = --MEM32(self + 4);
    if (!n) { kbd_t *d = DEV(self); if (d) memset(d, 0, sizeof *d); hle_com_free(self); }
    RET(n, 1);
}
static void dv_GetCapabilities(void) {                   /* (this, DIDEVCAPS*) */
    uint32_t c = A32(1);
    if (!c) RET(DIERR_INVALIDPARAM, 2);
    uint32_t size = MEM32(c);
    if (size > 4) memset(hle_ptr(c + 4), 0, size - 4);
    MEM32(c + 4) = 1u;                                   /* DIDC_ATTACHED */
    MEM32(c + 8) = DIDEVTYPE_KEYBOARD | DIDEVTYPEKEYBOARD_PCENH << 8;
    MEM32(c + 12) = 0, MEM32(c + 16) = 128, MEM32(c + 20) = 0;   /* axes, buttons, POVs */
    RET(DI_OK, 2);
}
static void dv_EnumObjects(void) { sfcop_todo_once("IDirectInputDevice::EnumObjects", "no objects enumerated"); RET(DI_OK, 4); }
static void dv_GetProperty(void) {                       /* (this, guid, DIPROPHEADER*) */
    kbd_t *d = DEV(A32(0));
    if (d && A32(1) == DIPROP_BUFFERSIZE && A32(2)) { MEM32(A32(2) + 16) = (uint32_t)d->buffer_size; RET(DI_OK, 3); }
    RET(DIERR_UNSUPPORTED, 3);
}
static void dv_SetProperty(void) {                       /* (this, guid, DIPROPHEADER*) */
    kbd_t *d = DEV(A32(0));
    if (d && A32(1) == DIPROP_BUFFERSIZE && A32(2)) {
        uint32_t n = MEM32(A32(2) + 16);
        d->buffer_size = n > RING ? RING : (int)n;
        RET(DI_OK, 3);
    }
    RET(DI_OK, 3);                                       /* axis modes, ranges: nothing on a keyboard */
}
static void dv_Acquire(void) {
    kbd_t *d = DEV(A32(0));
    if (!d) RET(DIERR_INVALIDPARAM, 1);
    if (d->acquired) RET(DI_NOEFFECT, 1);
    d->acquired = 1;
    snapshot(d->last);                                   /* keys already down are not events */
    d->count = d->head = 0, d->overflow = 0;
    RET(DI_OK, 1);
}
static void dv_Unacquire(void) {
    kbd_t *d = DEV(A32(0));
    if (!d) RET(DIERR_INVALIDPARAM, 1);
    if (!d->acquired) RET(DI_NOEFFECT, 1);
    d->acquired = 0;
    RET(DI_OK, 1);
}
static void dv_GetDeviceState(void) {                    /* (this, cbData, lpvData) */
    kbd_t *d = DEV(A32(0));
    uint32_t n = A32(1), p = A32(2);
    if (!d || !p) RET(DIERR_INVALIDPARAM, 3);
    if (!d->acquired) RET(DIERR_NOTACQUIRED, 3);
    uint8_t now[256];
    snapshot(now);
    memcpy(hle_ptr(p), now, n < 256 ? n : 256);
    if (n > 256) memset(hle_ptr(p + 256), 0, n - 256);
    RET(DI_OK, 3);
}
static void dv_GetDeviceData(void) {                     /* (this, cbObjectData, rgdod, &inout, flags) */
    kbd_t *d = DEV(A32(0));
    uint32_t sz = A32(1), out = A32(2), io = A32(3), flags = A32(4);
    if (!d || !io || (out && sz < 16)) RET(DIERR_INVALIDPARAM, 5);
    if (!d->acquired) RET(DIERR_NOTACQUIRED, 5);
    if (!d->buffer_size) RET(DIERR_NOTACQUIRED, 5);     /* DIERR_NOTBUFFERED has the same value */
    collect(d);
    uint32_t want = MEM32(io), got = 0;
    while (got < want && got < d->count) {
        uint32_t k = (d->head + got) % RING;
        if (out) {
            uint32_t e = out + got * sz;
            memset(hle_ptr(e), 0, sz);
            MEM32(e) = d->ev[k].ofs, MEM32(e + 4) = d->ev[k].data, MEM32(e + 8) = d->ev[k].time, MEM32(e + 12) = d->ev[k].seq;
        }
        got++;
    }
    if (!(flags & DIGDD_PEEK)) d->head = (d->head + got) % RING, d->count -= got;
    MEM32(io) = got;
    uint32_t hr = d->overflow ? DI_BUFFEROVERFLOW : DI_OK;
    d->overflow = 0;
    RET(hr, 5);
}
static void dv_SetDataFormat(void) {                     /* (this, DIDATAFORMAT*) */
    kbd_t *d = DEV(A32(0));
    uint32_t f = A32(1);
    if (!d || !f) RET(DIERR_INVALIDPARAM, 2);
    d->data_size = MEM32(f + 12);                        /* dwSize, dwObjSize, dwFlags, dwDataSize */
    if (d->data_size != 256) fprintf(stderr, "[dinput] keyboard data format of %u bytes: read as the 256-byte state\n", d->data_size);
    RET(DI_OK, 2);
}
static void dv_SetEventNotification(void) { sfcop_todo_once("IDirectInputDevice::SetEventNotification", "event never signalled"); RET(DI_OK, 2); }
static void dv_SetCooperativeLevel(void) { RET(DI_OK, 3); }
static void dv_GetObjectInfo(void) { RET(DIERR_OBJECTNOTFOUND, 4); }
static void fill_instance(uint32_t p) {                  /* a DIDEVICEINSTANCEA, as far as its dwSize goes */
    uint32_t size = MEM32(p);
    if (size < 8) return;
    uint8_t tmp[580];
    memset(tmp, 0, sizeof tmp);
    *(uint32_t *)tmp = size;
    memcpy(tmp + 4, GUID_SysKeyboard, 16);               /* guidInstance */
    memcpy(tmp + 20, GUID_SysKeyboard, 16);              /* guidProduct */
    *(uint32_t *)(tmp + 36) = DIDEVTYPE_KEYBOARD | DIDEVTYPEKEYBOARD_PCENH << 8;
    strcpy((char *)tmp + 40, "Keyboard");                /* tszInstanceName[260] */
    strcpy((char *)tmp + 300, "Keyboard");               /* tszProductName[260] */
    memcpy(hle_ptr(p + 4), tmp + 4, (size < sizeof tmp ? size : sizeof tmp) - 4);
}
static void dv_GetDeviceInfo(void) {
    if (!A32(1)) RET(DIERR_INVALIDPARAM, 2);
    fill_instance(A32(1));
    RET(DI_OK, 2);
}
static void dv_RunControlPanel(void) { RET(DI_OK, 3); }
static void dv_Initialize(void) { RET(DI_OK, 4); }
/* IDirectInputDevice2/7: force feedback and the rest, which a keyboard has not. */
static void dv_unsupported_2(void) { RET(DIERR_UNSUPPORTED, 2); }
static void dv_unsupported_3(void) { RET(DIERR_UNSUPPORTED, 3); }
static void dv_unsupported_4(void) { RET(DIERR_UNSUPPORTED, 4); }
static void dv_unsupported_5(void) { RET(DIERR_UNSUPPORTED, 5); }
static void dv_CreateEffect(void) { if (A32(3)) MEM32(A32(3)) = 0; RET(DIERR_UNSUPPORTED, 5); }
static void dv_EnumEffects(void) { RET(DI_OK, 4); }
static void dv_GetForceFeedbackState(void) { RET(DIERR_UNSUPPORTED, 2); }
static void dv_Poll(void) { RET(DEV(A32(0)) && DEV(A32(0))->acquired ? DI_NOEFFECT : DIERR_NOTACQUIRED, 1); }
static void dv_SendDeviceData(void) { RET(DIERR_UNSUPPORTED, 5); }

/* ---- IDirectInputA ---- */
static uint32_t new_keyboard(void) {
    for (int i = 0; i < MAX_DEVS; i++)
        if (!g_dev[i].used) {
            memset(&g_dev[i], 0, sizeof g_dev[i]);
            g_dev[i].used = 1;
            uint32_t o = hle_com_new(g_vt_dev, 12);
            MEM32(o + 8) = (uint32_t)i;
            return o;
        }
    return 0;
}
static uint32_t create_device(uint32_t guid, uint32_t out) {
    if (!out) return DIERR_INVALIDPARAM;
    MEM32(out) = 0;
    if (!sfcop_guid_eq(guid, GUID_SysKeyboard)) {
        char s[40] = "(null)";
        if (guid) hle_guid_string((const uint8_t *)hle_ptr(guid), s);
        fprintf(stderr, "[dinput] CreateDevice(%s): only the system keyboard is served\n", s);
        return DIERR_DEVICENOTREG;
    }
    uint32_t o = new_keyboard();
    if (!o) return DIERR_INVALIDPARAM;
    MEM32(out) = o;
    return DI_OK;
}
static void di_Release(void) {
    uint32_t self = A32(0), n = --MEM32(self + 4);
    if (!n) hle_com_free(self);
    RET(n, 1);
}
static void di_CreateDevice(void) { RET(create_device(A32(1), A32(2)), 4); }        /* (this, guid, &out, outer) */
static void di_CreateDeviceEx(void) { RET(create_device(A32(1), A32(3)), 5); }      /* (this, guid, iid, &out, outer) */
static void di_EnumDevices(void) {                       /* (this, devtype, callback, ref, flags) */
    uint32_t type = A32(1) & 0xFF, cb = A32(2);
    if (!cb) RET(DIERR_INVALIDPARAM, 5);
    if (type == 0 || type == DIDEVTYPE_KEYBOARD) {
        uint32_t mark = hle_scratch_mark(), inst = hle_scratch(580);
        MEM32(inst) = 580;
        fill_instance(inst);
        uint32_t a[2] = { inst, A32(3) };
        hle_call_guest(cb, 2, a);
        hle_scratch_release(mark);
    }
    RET(DI_OK, 5);
}
static void di_GetDeviceStatus(void) { RET(sfcop_guid_eq(A32(1), GUID_SysKeyboard) ? DI_OK : DIERR_DEVICENOTREG, 2); }
static void di_RunControlPanel(void) { RET(DI_OK, 3); }
static void di_Initialize(void) { RET(DI_OK, 3); }
static void di_FindDevice(void) { RET(DIERR_DEVICENOTREG, 4); }

static void vtables(void) {
    if (g_vt_di) return;
    static const win32hle_shim di[] = {
        { "IDirectInputA::QueryInterface", hle_com_QueryInterface }, { "IDirectInputA::AddRef", hle_com_AddRef },
        { "IDirectInputA::Release", di_Release }, { "IDirectInputA::CreateDevice", di_CreateDevice },
        { "IDirectInputA::EnumDevices", di_EnumDevices }, { "IDirectInputA::GetDeviceStatus", di_GetDeviceStatus },
        { "IDirectInputA::RunControlPanel", di_RunControlPanel }, { "IDirectInputA::Initialize", di_Initialize },
        { "IDirectInput2A::FindDevice", di_FindDevice }, { "IDirectInput7A::CreateDeviceEx", di_CreateDeviceEx },
        { 0, 0 } };
    static const win32hle_shim dev[] = {
        { "IDirectInputDeviceA::QueryInterface", hle_com_QueryInterface }, { "IDirectInputDeviceA::AddRef", hle_com_AddRef },
        { "IDirectInputDeviceA::Release", dv_Release }, { "IDirectInputDeviceA::GetCapabilities", dv_GetCapabilities },
        { "IDirectInputDeviceA::EnumObjects", dv_EnumObjects }, { "IDirectInputDeviceA::GetProperty", dv_GetProperty },
        { "IDirectInputDeviceA::SetProperty", dv_SetProperty }, { "IDirectInputDeviceA::Acquire", dv_Acquire },
        { "IDirectInputDeviceA::Unacquire", dv_Unacquire }, { "IDirectInputDeviceA::GetDeviceState", dv_GetDeviceState },
        { "IDirectInputDeviceA::GetDeviceData", dv_GetDeviceData }, { "IDirectInputDeviceA::SetDataFormat", dv_SetDataFormat },
        { "IDirectInputDeviceA::SetEventNotification", dv_SetEventNotification },
        { "IDirectInputDeviceA::SetCooperativeLevel", dv_SetCooperativeLevel },
        { "IDirectInputDeviceA::GetObjectInfo", dv_GetObjectInfo }, { "IDirectInputDeviceA::GetDeviceInfo", dv_GetDeviceInfo },
        { "IDirectInputDeviceA::RunControlPanel", dv_RunControlPanel }, { "IDirectInputDeviceA::Initialize", dv_Initialize },
        { "IDirectInputDevice2A::CreateEffect", dv_CreateEffect }, { "IDirectInputDevice2A::EnumEffects", dv_EnumEffects },
        { "IDirectInputDevice2A::GetEffectInfo", dv_unsupported_3 },
        { "IDirectInputDevice2A::GetForceFeedbackState", dv_GetForceFeedbackState },
        { "IDirectInputDevice2A::SendForceFeedbackCommand", dv_unsupported_2 },
        { "IDirectInputDevice2A::EnumCreatedEffectObjects", dv_unsupported_4 },
        { "IDirectInputDevice2A::Escape", dv_unsupported_2 }, { "IDirectInputDevice2A::Poll", dv_Poll },
        { "IDirectInputDevice2A::SendDeviceData", dv_SendDeviceData },
        { "IDirectInputDevice7A::EnumEffectsInFile", dv_unsupported_5 },
        { "IDirectInputDevice7A::WriteEffectToFile", dv_unsupported_5 }, { 0, 0 } };
    g_vt_di = hle_com_vtable(di);
    g_vt_dev = hle_com_vtable(dev);
}

static void s_DirectInputCreateA(void) {                 /* (hinst, version, &out, outer) */
    vtables();
    if (!A32(2)) RET(DIERR_INVALIDPARAM, 4);
    MEM32(A32(2)) = hle_com_new(g_vt_di, 8);
    RET(DI_OK, 4);
}

const win32hle_shim sfcop_hle_dinput[] = {
    { "DirectInputCreateA", s_DirectInputCreateA },
    { 0, 0 }
};
