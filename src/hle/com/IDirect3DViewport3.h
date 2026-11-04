/* IDirect3DViewport3: method order and IID from the public header include/d3d.h of Wine 9.0.
 * Written for this project in its own form; obtained from IDirect3D3::CreateViewport. */
#ifndef SFCOP_HLE_COM_IDIRECT3DVIEWPORT3_H
#define SFCOP_HLE_COM_IDIRECT3DVIEWPORT3_H

#include "D3DTypes.h"

SFCOP_COM_BEGIN

/* {B0AB3B61-33D7-11D1-A981-00C04FD7B174} */
static const GUID kIID_IDirect3DViewport3 = {0xB0AB3B61, 0x33D7, 0x11D1, {0xA9, 0x81, 0x00, 0xC0, 0x4F, 0xD7, 0xB1, 0x74}};

typedef struct IDirect3DViewport3Vtbl {
    // clang-format off
    /*  0 */ HRESULT (SFCOP_STDCALL *QueryInterface)(IDirect3DViewport3 *self, REFIID riid, void **object);
    /*  1 */ ULONG (SFCOP_STDCALL *AddRef)(IDirect3DViewport3 *self);
    /*  2 */ ULONG (SFCOP_STDCALL *Release)(IDirect3DViewport3 *self);
    /*  3 */ HRESULT (SFCOP_STDCALL *Initialize)(IDirect3DViewport3 *self, IDirect3D *d3d);
    /*  4 */ HRESULT (SFCOP_STDCALL *GetViewport)(IDirect3DViewport3 *self, D3DVIEWPORT *data);
    /*  5 */ HRESULT (SFCOP_STDCALL *SetViewport)(IDirect3DViewport3 *self, D3DVIEWPORT *data);
    /*  6 */ HRESULT (SFCOP_STDCALL *TransformVertices)(IDirect3DViewport3 *self, DWORD vertex_count, D3DTRANSFORMDATA *data, DWORD flags, DWORD *offscreen);
    /*  7 */ HRESULT (SFCOP_STDCALL *LightElements)(IDirect3DViewport3 *self, DWORD element_count, D3DLIGHTDATA *data);
    /*  8 */ HRESULT (SFCOP_STDCALL *SetBackground)(IDirect3DViewport3 *self, D3DMATERIALHANDLE material);
    /*  9 */ HRESULT (SFCOP_STDCALL *GetBackground)(IDirect3DViewport3 *self, D3DMATERIALHANDLE *material, BOOL *valid);
    /* 10 */ HRESULT (SFCOP_STDCALL *SetBackgroundDepth)(IDirect3DViewport3 *self, IDirectDrawSurface *surface);
    /* 11 */ HRESULT (SFCOP_STDCALL *GetBackgroundDepth)(IDirect3DViewport3 *self, IDirectDrawSurface **surface, BOOL *valid);
    /* 12 */ HRESULT (SFCOP_STDCALL *Clear)(IDirect3DViewport3 *self, DWORD count, D3DRECT *rects, DWORD flags);
    /* 13 */ HRESULT (SFCOP_STDCALL *AddLight)(IDirect3DViewport3 *self, IDirect3DLight *light);
    /* 14 */ HRESULT (SFCOP_STDCALL *DeleteLight)(IDirect3DViewport3 *self, IDirect3DLight *light);
    /* 15 */ HRESULT (SFCOP_STDCALL *NextLight)(IDirect3DViewport3 *self, IDirect3DLight *reference, IDirect3DLight **light, DWORD flags);
    /* 16 */ HRESULT (SFCOP_STDCALL *GetViewport2)(IDirect3DViewport3 *self, D3DVIEWPORT2 *data);
    /* 17 */ HRESULT (SFCOP_STDCALL *SetViewport2)(IDirect3DViewport3 *self, D3DVIEWPORT2 *data);
    /* 18 */ HRESULT (SFCOP_STDCALL *SetBackgroundDepth2)(IDirect3DViewport3 *self, IDirectDrawSurface4 *surface);
    /* 19 */ HRESULT (SFCOP_STDCALL *GetBackgroundDepth2)(IDirect3DViewport3 *self, IDirectDrawSurface4 **surface, BOOL *valid);
    /* 20 */ HRESULT (SFCOP_STDCALL *Clear2)(IDirect3DViewport3 *self, DWORD count, D3DRECT *rects, DWORD flags, D3DCOLOR color, D3DVALUE z, DWORD stencil);
    // clang-format on
} IDirect3DViewport3Vtbl;
struct IDirect3DViewport3 {
    const IDirect3DViewport3Vtbl *lpVtbl;
};
SFCOP_COM_ASSERT_SLOTS(IDirect3DViewport3Vtbl, 21);

SFCOP_COM_END

#endif
