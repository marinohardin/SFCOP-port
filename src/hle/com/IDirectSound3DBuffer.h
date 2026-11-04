/* IDirectSound3DBuffer: method order and IID from the public header include/dsound.h of Wine 9.0.
 * Written for this project in its own form; obtained from IDirectSoundBuffer::QueryInterface. */
#ifndef SFCOP_HLE_COM_IDIRECTSOUND3DBUFFER_H
#define SFCOP_HLE_COM_IDIRECTSOUND3DBUFFER_H

#include "DSoundTypes.h"

SFCOP_COM_BEGIN

/* {279AFA86-4981-11CE-A521-0020AF0BE560} */
static const GUID kIID_IDirectSound3DBuffer = {0x279AFA86, 0x4981, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};

typedef struct IDirectSound3DBufferVtbl {
    // clang-format off
    /*  0 */ HRESULT (SFCOP_STDCALL *QueryInterface)(IDirectSound3DBuffer *self, REFIID riid, void **object);
    /*  1 */ ULONG (SFCOP_STDCALL *AddRef)(IDirectSound3DBuffer *self);
    /*  2 */ ULONG (SFCOP_STDCALL *Release)(IDirectSound3DBuffer *self);
    /*  3 */ HRESULT (SFCOP_STDCALL *GetAllParameters)(IDirectSound3DBuffer *self, DS3DBUFFER *params);
    /*  4 */ HRESULT (SFCOP_STDCALL *GetConeAngles)(IDirectSound3DBuffer *self, DWORD *inside_angle, DWORD *outside_angle);
    /*  5 */ HRESULT (SFCOP_STDCALL *GetConeOrientation)(IDirectSound3DBuffer *self, D3DVECTOR *orientation);
    /*  6 */ HRESULT (SFCOP_STDCALL *GetConeOutsideVolume)(IDirectSound3DBuffer *self, LONG *volume);
    /*  7 */ HRESULT (SFCOP_STDCALL *GetMaxDistance)(IDirectSound3DBuffer *self, D3DVALUE *distance);
    /*  8 */ HRESULT (SFCOP_STDCALL *GetMinDistance)(IDirectSound3DBuffer *self, D3DVALUE *distance);
    /*  9 */ HRESULT (SFCOP_STDCALL *GetMode)(IDirectSound3DBuffer *self, DWORD *mode);
    /* 10 */ HRESULT (SFCOP_STDCALL *GetPosition)(IDirectSound3DBuffer *self, D3DVECTOR *position);
    /* 11 */ HRESULT (SFCOP_STDCALL *GetVelocity)(IDirectSound3DBuffer *self, D3DVECTOR *velocity);
    /* 12 */ HRESULT (SFCOP_STDCALL *SetAllParameters)(IDirectSound3DBuffer *self, const DS3DBUFFER *params, DWORD apply);
    /* 13 */ HRESULT (SFCOP_STDCALL *SetConeAngles)(IDirectSound3DBuffer *self, DWORD inside_angle, DWORD outside_angle, DWORD apply);
    /* 14 */ HRESULT (SFCOP_STDCALL *SetConeOrientation)(IDirectSound3DBuffer *self, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply);
    /* 15 */ HRESULT (SFCOP_STDCALL *SetConeOutsideVolume)(IDirectSound3DBuffer *self, LONG volume, DWORD apply);
    /* 16 */ HRESULT (SFCOP_STDCALL *SetMaxDistance)(IDirectSound3DBuffer *self, D3DVALUE distance, DWORD apply);
    /* 17 */ HRESULT (SFCOP_STDCALL *SetMinDistance)(IDirectSound3DBuffer *self, D3DVALUE distance, DWORD apply);
    /* 18 */ HRESULT (SFCOP_STDCALL *SetMode)(IDirectSound3DBuffer *self, DWORD mode, DWORD apply);
    /* 19 */ HRESULT (SFCOP_STDCALL *SetPosition)(IDirectSound3DBuffer *self, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply);
    /* 20 */ HRESULT (SFCOP_STDCALL *SetVelocity)(IDirectSound3DBuffer *self, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply);
    // clang-format on
} IDirectSound3DBufferVtbl;
struct IDirectSound3DBuffer {
    const IDirectSound3DBufferVtbl *lpVtbl;
};
SFCOP_COM_ASSERT_SLOTS(IDirectSound3DBufferVtbl, 21);

SFCOP_COM_END

#endif
