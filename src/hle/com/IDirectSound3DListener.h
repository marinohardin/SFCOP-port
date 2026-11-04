/* IDirectSound3DListener: method order and IID from the public header include/dsound.h of Wine 9.0.
 * Written for this project in its own form; obtained from IDirectSoundBuffer::QueryInterface. */
#ifndef SFCOP_HLE_COM_IDIRECTSOUND3DLISTENER_H
#define SFCOP_HLE_COM_IDIRECTSOUND3DLISTENER_H

#include "DSoundTypes.h"

SFCOP_COM_BEGIN

/* {279AFA84-4981-11CE-A521-0020AF0BE560} */
static const GUID kIID_IDirectSound3DListener = {0x279AFA84, 0x4981, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};

typedef struct IDirectSound3DListenerVtbl {
    // clang-format off
    /*  0 */ HRESULT (SFCOP_STDCALL *QueryInterface)(IDirectSound3DListener *self, REFIID riid, void **object);
    /*  1 */ ULONG (SFCOP_STDCALL *AddRef)(IDirectSound3DListener *self);
    /*  2 */ ULONG (SFCOP_STDCALL *Release)(IDirectSound3DListener *self);
    /*  3 */ HRESULT (SFCOP_STDCALL *GetAllParameters)(IDirectSound3DListener *self, DS3DLISTENER *params);
    /*  4 */ HRESULT (SFCOP_STDCALL *GetDistanceFactor)(IDirectSound3DListener *self, D3DVALUE *factor);
    /*  5 */ HRESULT (SFCOP_STDCALL *GetDopplerFactor)(IDirectSound3DListener *self, D3DVALUE *factor);
    /*  6 */ HRESULT (SFCOP_STDCALL *GetOrientation)(IDirectSound3DListener *self, D3DVECTOR *front, D3DVECTOR *top);
    /*  7 */ HRESULT (SFCOP_STDCALL *GetPosition)(IDirectSound3DListener *self, D3DVECTOR *position);
    /*  8 */ HRESULT (SFCOP_STDCALL *GetRolloffFactor)(IDirectSound3DListener *self, D3DVALUE *factor);
    /*  9 */ HRESULT (SFCOP_STDCALL *GetVelocity)(IDirectSound3DListener *self, D3DVECTOR *velocity);
    /* 10 */ HRESULT (SFCOP_STDCALL *SetAllParameters)(IDirectSound3DListener *self, const DS3DLISTENER *params, DWORD apply);
    /* 11 */ HRESULT (SFCOP_STDCALL *SetDistanceFactor)(IDirectSound3DListener *self, D3DVALUE factor, DWORD apply);
    /* 12 */ HRESULT (SFCOP_STDCALL *SetDopplerFactor)(IDirectSound3DListener *self, D3DVALUE factor, DWORD apply);
    /* 13 */ HRESULT (SFCOP_STDCALL *SetOrientation)(IDirectSound3DListener *self, D3DVALUE front_x, D3DVALUE front_y, D3DVALUE front_z, D3DVALUE top_x, D3DVALUE top_y, D3DVALUE top_z, DWORD apply);
    /* 14 */ HRESULT (SFCOP_STDCALL *SetPosition)(IDirectSound3DListener *self, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply);
    /* 15 */ HRESULT (SFCOP_STDCALL *SetRolloffFactor)(IDirectSound3DListener *self, D3DVALUE factor, DWORD apply);
    /* 16 */ HRESULT (SFCOP_STDCALL *SetVelocity)(IDirectSound3DListener *self, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply);
    /* 17 */ HRESULT (SFCOP_STDCALL *CommitDeferredSettings)(IDirectSound3DListener *self);
    // clang-format on
} IDirectSound3DListenerVtbl;
struct IDirectSound3DListener {
    const IDirectSound3DListenerVtbl *lpVtbl;
};
SFCOP_COM_ASSERT_SLOTS(IDirectSound3DListenerVtbl, 18);

SFCOP_COM_END

#endif
