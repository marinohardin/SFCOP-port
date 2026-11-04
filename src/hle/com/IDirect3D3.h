/* IDirect3D3: method order and IID from the public header include/d3d.h of Wine 9.0.
 * Written for this project in its own form; obtained from IDirectDraw4::QueryInterface. */
#ifndef SFCOP_HLE_COM_IDIRECT3D3_H
#define SFCOP_HLE_COM_IDIRECT3D3_H

#include "D3DTypes.h"

SFCOP_COM_BEGIN

/* {BB223240-E72B-11D0-A9B4-00AA00C0993E} */
static const GUID kIID_IDirect3D3 = {0xBB223240, 0xE72B, 0x11D0, {0xA9, 0xB4, 0x00, 0xAA, 0x00, 0xC0, 0x99, 0x3E}};

typedef struct IDirect3D3Vtbl {
    // clang-format off
    /*  0 */ HRESULT (SFCOP_STDCALL *QueryInterface)(IDirect3D3 *self, REFIID riid, void **object);
    /*  1 */ ULONG (SFCOP_STDCALL *AddRef)(IDirect3D3 *self);
    /*  2 */ ULONG (SFCOP_STDCALL *Release)(IDirect3D3 *self);
    /*  3 */ HRESULT (SFCOP_STDCALL *EnumDevices)(IDirect3D3 *self, LPD3DENUMDEVICESCALLBACK callback, void *context);
    /*  4 */ HRESULT (SFCOP_STDCALL *CreateLight)(IDirect3D3 *self, IDirect3DLight **light, IUnknown *outer);
    /*  5 */ HRESULT (SFCOP_STDCALL *CreateMaterial)(IDirect3D3 *self, IDirect3DMaterial3 **material, IUnknown *outer);
    /*  6 */ HRESULT (SFCOP_STDCALL *CreateViewport)(IDirect3D3 *self, IDirect3DViewport3 **viewport, IUnknown *outer);
    /*  7 */ HRESULT (SFCOP_STDCALL *FindDevice)(IDirect3D3 *self, D3DFINDDEVICESEARCH *search, D3DFINDDEVICERESULT *result);
    /*  8 */ HRESULT (SFCOP_STDCALL *CreateDevice)(IDirect3D3 *self, REFCLSID device_clsid, IDirectDrawSurface4 *target, IDirect3DDevice3 **device, IUnknown *outer);
    /*  9 */ HRESULT (SFCOP_STDCALL *CreateVertexBuffer)(IDirect3D3 *self, D3DVERTEXBUFFERDESC *desc, IDirect3DVertexBuffer **buffer, DWORD flags, IUnknown *outer);
    /* 10 */ HRESULT (SFCOP_STDCALL *EnumZBufferFormats)(IDirect3D3 *self, REFCLSID device_clsid, LPD3DENUMPIXELFORMATSCALLBACK callback, void *context);
    /* 11 */ HRESULT (SFCOP_STDCALL *EvictManagedTextures)(IDirect3D3 *self);
    // clang-format on
} IDirect3D3Vtbl;
struct IDirect3D3 {
    const IDirect3D3Vtbl *lpVtbl;
};
SFCOP_COM_ASSERT_SLOTS(IDirect3D3Vtbl, 12);

SFCOP_COM_END

#endif
