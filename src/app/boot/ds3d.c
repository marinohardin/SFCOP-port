// TEMPORARY: replaced by Wave 1 B4 (audio)
/*
 * ds3d.c - IDirectSound3DBuffer and IDirectSound3DListener, accepted and
 * stored, not yet heard. Upstreamable: yes.
 *
 * win32hle's dsound.c answers IDirectSoundBuffer::QueryInterface with the
 * buffer itself for any interface, so a 3D call would land on a 2D slot.
 * This overrides that QueryInterface: IID_IDirectSound3DBuffer gets a 3D
 * buffer object and IID_IDirectSound3DListener a listener; anything else is
 * handed to the original. Every Set stores its values, every Get returns
 * them, and nothing changes what the mixer plays. TODO table (README.md,
 * "DirectSound3D"): positional attenuation, pan from position, cone volume,
 * Doppler; DS3D_DEFERRED settings are applied at once.
 */
#include <stdio.h>
#include <string.h>
#include "sfcop_hle.h"

#define DS_OK              0u
#define DSERR_INVALIDPARAM 0x80070057u

static const uint8_t IID_IDirectSound3DBuffer[16] = { 0x86, 0xFA, 0x9A, 0x27, 0x81, 0x49, 0xCE, 0x11,
                                                      0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 };
static const uint8_t IID_IDirectSound3DListener[16] = { 0x84, 0xFA, 0x9A, 0x27, 0x81, 0x49, 0xCE, 0x11,
                                                        0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 };

/* Both objects: vtable, refs, then the parameter block the Get methods read,
 * laid out as DS3DBUFFER / DS3DLISTENER are (dwSize first). */
#define PARAMS 8u
/* DS3DBUFFER: size, position(3f), velocity(3f), inside, outside, orientation(3f), outside volume, min, max, mode */
enum { B_POS = 4, B_VEL = 16, B_IN = 28, B_OUT = 32, B_ORIENT = 36, B_OUTVOL = 48, B_MIN = 52, B_MAX = 56, B_MODE = 60, B_SIZE = 64 };
/* DS3DLISTENER: size, position(3f), velocity(3f), front(3f), top(3f), distance, rolloff, doppler */
enum { L_POS = 4, L_VEL = 16, L_FRONT = 28, L_TOP = 40, L_DIST = 52, L_ROLL = 56, L_DOPP = 60, L_SIZE = 64 };

static uint32_t g_vt_buf3d, g_vt_lis;
static void put(uint32_t self, uint32_t off, int nargs) {     /* copy nargs dwords (args 1..n) into the block */
    for (int i = 0; i < nargs; i++) MEM32(self + PARAMS + off + 4u * (uint32_t)i) = A32(1 + i);
}
static void get(uint32_t self, uint32_t off, uint32_t dst, int n) {
    if (dst) for (int i = 0; i < n; i++) MEM32(dst + 4u * (uint32_t)i) = MEM32(self + PARAMS + off + 4u * (uint32_t)i);
}

/* ---- IDirectSound3DBuffer ---- */
#define BSET(fn, off, n) static void fn(void) { put(A32(0), off, n); RET(DS_OK, (n) + 2); }
#define BGET(fn, off, n) static void fn(void) { get(A32(0), off, A32(1), n); RET(A32(1) ? DS_OK : DSERR_INVALIDPARAM, 2); }
BSET(b3_SetPosition, B_POS, 3)
BSET(b3_SetVelocity, B_VEL, 3)
BSET(b3_SetConeAngles, B_IN, 2)
BSET(b3_SetConeOrientation, B_ORIENT, 3)
BSET(b3_SetConeOutsideVolume, B_OUTVOL, 1)
BSET(b3_SetMinDistance, B_MIN, 1)
BSET(b3_SetMaxDistance, B_MAX, 1)
BSET(b3_SetMode, B_MODE, 1)
BGET(b3_GetPosition, B_POS, 3)
BGET(b3_GetVelocity, B_VEL, 3)
BGET(b3_GetConeOrientation, B_ORIENT, 3)
BGET(b3_GetConeOutsideVolume, B_OUTVOL, 1)
BGET(b3_GetMinDistance, B_MIN, 1)
BGET(b3_GetMaxDistance, B_MAX, 1)
BGET(b3_GetMode, B_MODE, 1)
static void b3_GetConeAngles(void) {                     /* (this, &inside, &outside) */
    get(A32(0), B_IN, A32(1), 1), get(A32(0), B_OUT, A32(2), 1);
    RET(DS_OK, 3);
}
static void b3_GetAllParameters(void) {                  /* (this, DS3DBUFFER*) */
    uint32_t self = A32(0), p = A32(1);
    if (!p || MEM32(p) < B_SIZE) RET(DSERR_INVALIDPARAM, 2);
    memcpy(hle_ptr(p + 4), hle_ptr(self + PARAMS + 4), B_SIZE - 4);
    RET(DS_OK, 2);
}
static void b3_SetAllParameters(void) {                  /* (this, DS3DBUFFER*, apply) */
    uint32_t self = A32(0), p = A32(1);
    if (!p || MEM32(p) < B_SIZE) RET(DSERR_INVALIDPARAM, 3);
    memcpy(hle_ptr(self + PARAMS + 4), hle_ptr(p + 4), B_SIZE - 4);
    RET(DS_OK, 3);
}

/* ---- IDirectSound3DListener ---- */
BSET(l_SetPosition, L_POS, 3)
BSET(l_SetVelocity, L_VEL, 3)
BSET(l_SetOrientation, L_FRONT, 6)
BSET(l_SetDistanceFactor, L_DIST, 1)
BSET(l_SetRolloffFactor, L_ROLL, 1)
BSET(l_SetDopplerFactor, L_DOPP, 1)
BGET(l_GetPosition, L_POS, 3)
BGET(l_GetVelocity, L_VEL, 3)
BGET(l_GetDistanceFactor, L_DIST, 1)
BGET(l_GetRolloffFactor, L_ROLL, 1)
BGET(l_GetDopplerFactor, L_DOPP, 1)
static void l_GetOrientation(void) {                     /* (this, &front, &top) */
    get(A32(0), L_FRONT, A32(1), 3), get(A32(0), L_TOP, A32(2), 3);
    RET(DS_OK, 3);
}
static void l_GetAllParameters(void) {
    uint32_t self = A32(0), p = A32(1);
    if (!p || MEM32(p) < L_SIZE) RET(DSERR_INVALIDPARAM, 2);
    memcpy(hle_ptr(p + 4), hle_ptr(self + PARAMS + 4), L_SIZE - 4);
    RET(DS_OK, 2);
}
static void l_SetAllParameters(void) {
    uint32_t self = A32(0), p = A32(1);
    if (!p || MEM32(p) < L_SIZE) RET(DSERR_INVALIDPARAM, 3);
    memcpy(hle_ptr(self + PARAMS + 4), hle_ptr(p + 4), L_SIZE - 4);
    RET(DS_OK, 3);
}
static void l_CommitDeferredSettings(void) { RET(DS_OK, 1); }

static void vtables(void) {
    if (g_vt_buf3d) return;
    static const win32hle_shim b3[] = {
        { "IDirectSound3DBuffer::QueryInterface", hle_com_QueryInterface }, { "IDirectSound3DBuffer::AddRef", hle_com_AddRef },
        { "IDirectSound3DBuffer::Release", hle_com_Release },
        { "IDirectSound3DBuffer::GetAllParameters", b3_GetAllParameters }, { "IDirectSound3DBuffer::GetConeAngles", b3_GetConeAngles },
        { "IDirectSound3DBuffer::GetConeOrientation", b3_GetConeOrientation },
        { "IDirectSound3DBuffer::GetConeOutsideVolume", b3_GetConeOutsideVolume },
        { "IDirectSound3DBuffer::GetMaxDistance", b3_GetMaxDistance }, { "IDirectSound3DBuffer::GetMinDistance", b3_GetMinDistance },
        { "IDirectSound3DBuffer::GetMode", b3_GetMode }, { "IDirectSound3DBuffer::GetPosition", b3_GetPosition },
        { "IDirectSound3DBuffer::GetVelocity", b3_GetVelocity },
        { "IDirectSound3DBuffer::SetAllParameters", b3_SetAllParameters }, { "IDirectSound3DBuffer::SetConeAngles", b3_SetConeAngles },
        { "IDirectSound3DBuffer::SetConeOrientation", b3_SetConeOrientation },
        { "IDirectSound3DBuffer::SetConeOutsideVolume", b3_SetConeOutsideVolume },
        { "IDirectSound3DBuffer::SetMaxDistance", b3_SetMaxDistance }, { "IDirectSound3DBuffer::SetMinDistance", b3_SetMinDistance },
        { "IDirectSound3DBuffer::SetMode", b3_SetMode }, { "IDirectSound3DBuffer::SetPosition", b3_SetPosition },
        { "IDirectSound3DBuffer::SetVelocity", b3_SetVelocity }, { 0, 0 } };
    static const win32hle_shim lis[] = {
        { "IDirectSound3DListener::QueryInterface", hle_com_QueryInterface }, { "IDirectSound3DListener::AddRef", hle_com_AddRef },
        { "IDirectSound3DListener::Release", hle_com_Release },
        { "IDirectSound3DListener::GetAllParameters", l_GetAllParameters },
        { "IDirectSound3DListener::GetDistanceFactor", l_GetDistanceFactor },
        { "IDirectSound3DListener::GetDopplerFactor", l_GetDopplerFactor },
        { "IDirectSound3DListener::GetOrientation", l_GetOrientation }, { "IDirectSound3DListener::GetPosition", l_GetPosition },
        { "IDirectSound3DListener::GetRolloffFactor", l_GetRolloffFactor }, { "IDirectSound3DListener::GetVelocity", l_GetVelocity },
        { "IDirectSound3DListener::SetAllParameters", l_SetAllParameters },
        { "IDirectSound3DListener::SetDistanceFactor", l_SetDistanceFactor },
        { "IDirectSound3DListener::SetDopplerFactor", l_SetDopplerFactor },
        { "IDirectSound3DListener::SetOrientation", l_SetOrientation }, { "IDirectSound3DListener::SetPosition", l_SetPosition },
        { "IDirectSound3DListener::SetRolloffFactor", l_SetRolloffFactor }, { "IDirectSound3DListener::SetVelocity", l_SetVelocity },
        { "IDirectSound3DListener::CommitDeferredSettings", l_CommitDeferredSettings }, { 0, 0 } };
    g_vt_buf3d = hle_com_vtable(b3);
    g_vt_lis = hle_com_vtable(lis);
}

static const float k_one = 1.0f, k_big = 1000000000.0f;
static uint32_t f32(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static uint32_t new_3d(int listener) {
    vtables();
    uint32_t o = hle_com_new(listener ? g_vt_lis : g_vt_buf3d, PARAMS + 64);
    uint32_t p = o + PARAMS;
    MEM32(p) = 64;
    if (listener) {
        MEM32(p + L_FRONT + 8) = f32(k_one);              /* front (0,0,1), top (0,1,0) */
        MEM32(p + L_TOP + 4) = f32(k_one);
        MEM32(p + L_DIST) = f32(k_one), MEM32(p + L_ROLL) = f32(k_one), MEM32(p + L_DOPP) = f32(k_one);
    } else {
        MEM32(p + B_IN) = 360, MEM32(p + B_OUT) = 360;
        MEM32(p + B_ORIENT + 8) = f32(k_one);
        MEM32(p + B_MIN) = f32(k_one), MEM32(p + B_MAX) = f32(k_big);
    }
    return o;
}

/* IDirectSoundBuffer::QueryInterface(this, iid, &out) */
static void b_QueryInterface(void) {
    uint32_t iid = A32(1), out = A32(2);
    int lis = sfcop_guid_eq(iid, IID_IDirectSound3DListener);
    if (out && (lis || sfcop_guid_eq(iid, IID_IDirectSound3DBuffer))) {
        MEM32(out) = new_3d(lis);
        RET(DS_OK, 3);
    }
    hle_com_QueryInterface();
}

const win32hle_shim sfcop_spike_ds3d[] = {
    { "IDirectSoundBuffer::QueryInterface", b_QueryInterface },
    { 0, 0 }
};
