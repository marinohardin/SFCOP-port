/* IDirect3DTexture2: method order and IID from the public header include/d3d.h of Wine 9.0.
 * Written for this project in its own form; obtained from IDirectDrawSurface4::QueryInterface. */
#ifndef SFCOP_HLE_COM_IDIRECT3DTEXTURE2_H
#define SFCOP_HLE_COM_IDIRECT3DTEXTURE2_H

#include "D3DTypes.h"

SFCOP_COM_BEGIN

/* {93281502-8CF8-11D0-89AB-00A0C9054129} */
static const GUID kIID_IDirect3DTexture2 = {0x93281502, 0x8CF8, 0x11D0, {0x89, 0xAB, 0x00, 0xA0, 0xC9, 0x05, 0x41, 0x29}};

typedef struct IDirect3DTexture2Vtbl {
    // clang-format off
    /*  0 */ HRESULT (SFCOP_STDCALL *QueryInterface)(IDirect3DTexture2 *self, REFIID riid, void **object);
    /*  1 */ ULONG (SFCOP_STDCALL *AddRef)(IDirect3DTexture2 *self);
    /*  2 */ ULONG (SFCOP_STDCALL *Release)(IDirect3DTexture2 *self);
    /*  3 */ HRESULT (SFCOP_STDCALL *GetHandle)(IDirect3DTexture2 *self, IDirect3DDevice2 *device, D3DTEXTUREHANDLE *handle);
    /*  4 */ HRESULT (SFCOP_STDCALL *PaletteChanged)(IDirect3DTexture2 *self, DWORD start, DWORD count);
    /*  5 */ HRESULT (SFCOP_STDCALL *Load)(IDirect3DTexture2 *self, IDirect3DTexture2 *source);
    // clang-format on
} IDirect3DTexture2Vtbl;
struct IDirect3DTexture2 {
    const IDirect3DTexture2Vtbl *lpVtbl;
};
SFCOP_COM_ASSERT_SLOTS(IDirect3DTexture2Vtbl, 6);

SFCOP_COM_END

#endif
