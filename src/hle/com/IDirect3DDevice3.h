/* IDirect3DDevice3: method order and IID from the public header include/d3d.h of Wine 9.0.
 * Written for this project in its own form; obtained from IDirect3D3::CreateDevice. */
#ifndef SFCOP_HLE_COM_IDIRECT3DDEVICE3_H
#define SFCOP_HLE_COM_IDIRECT3DDEVICE3_H

#include "D3DTypes.h"

SFCOP_COM_BEGIN

/* {B0AB3B60-33D7-11D1-A981-00C04FD7B174} */
static const GUID kIID_IDirect3DDevice3 = {0xB0AB3B60, 0x33D7, 0x11D1, {0xA9, 0x81, 0x00, 0xC0, 0x4F, 0xD7, 0xB1, 0x74}};

typedef struct IDirect3DDevice3Vtbl {
    // clang-format off
    /*  0 */ HRESULT (SFCOP_STDCALL *QueryInterface)(IDirect3DDevice3 *self, REFIID riid, void **object);
    /*  1 */ ULONG (SFCOP_STDCALL *AddRef)(IDirect3DDevice3 *self);
    /*  2 */ ULONG (SFCOP_STDCALL *Release)(IDirect3DDevice3 *self);
    /*  3 */ HRESULT (SFCOP_STDCALL *GetCaps)(IDirect3DDevice3 *self, D3DDEVICEDESC *hal_desc, D3DDEVICEDESC *hel_desc);
    /*  4 */ HRESULT (SFCOP_STDCALL *GetStats)(IDirect3DDevice3 *self, D3DSTATS *stats);
    /*  5 */ HRESULT (SFCOP_STDCALL *AddViewport)(IDirect3DDevice3 *self, IDirect3DViewport3 *viewport);
    /*  6 */ HRESULT (SFCOP_STDCALL *DeleteViewport)(IDirect3DDevice3 *self, IDirect3DViewport3 *viewport);
    /*  7 */ HRESULT (SFCOP_STDCALL *NextViewport)(IDirect3DDevice3 *self, IDirect3DViewport3 *reference, IDirect3DViewport3 **viewport, DWORD flags);
    /*  8 */ HRESULT (SFCOP_STDCALL *EnumTextureFormats)(IDirect3DDevice3 *self, LPD3DENUMPIXELFORMATSCALLBACK callback, void *context);
    /*  9 */ HRESULT (SFCOP_STDCALL *BeginScene)(IDirect3DDevice3 *self);
    /* 10 */ HRESULT (SFCOP_STDCALL *EndScene)(IDirect3DDevice3 *self);
    /* 11 */ HRESULT (SFCOP_STDCALL *GetDirect3D)(IDirect3DDevice3 *self, IDirect3D3 **d3d);
    /* 12 */ HRESULT (SFCOP_STDCALL *SetCurrentViewport)(IDirect3DDevice3 *self, IDirect3DViewport3 *viewport);
    /* 13 */ HRESULT (SFCOP_STDCALL *GetCurrentViewport)(IDirect3DDevice3 *self, IDirect3DViewport3 **viewport);
    /* 14 */ HRESULT (SFCOP_STDCALL *SetRenderTarget)(IDirect3DDevice3 *self, IDirectDrawSurface4 *target, DWORD flags);
    /* 15 */ HRESULT (SFCOP_STDCALL *GetRenderTarget)(IDirect3DDevice3 *self, IDirectDrawSurface4 **target);
    /* 16 */ HRESULT (SFCOP_STDCALL *Begin)(IDirect3DDevice3 *self, D3DPRIMITIVETYPE primitive_type, DWORD vertex_format, DWORD flags);
    /* 17 */ HRESULT (SFCOP_STDCALL *BeginIndexed)(IDirect3DDevice3 *self, D3DPRIMITIVETYPE primitive_type, DWORD vertex_format, void *vertices, DWORD vertex_count, DWORD flags);
    /* 18 */ HRESULT (SFCOP_STDCALL *Vertex)(IDirect3DDevice3 *self, void *vertex);
    /* 19 */ HRESULT (SFCOP_STDCALL *Index)(IDirect3DDevice3 *self, WORD index);
    /* 20 */ HRESULT (SFCOP_STDCALL *End)(IDirect3DDevice3 *self, DWORD flags);
    /* 21 */ HRESULT (SFCOP_STDCALL *GetRenderState)(IDirect3DDevice3 *self, D3DRENDERSTATETYPE state_type, DWORD *value);
    /* 22 */ HRESULT (SFCOP_STDCALL *SetRenderState)(IDirect3DDevice3 *self, D3DRENDERSTATETYPE state_type, DWORD value);
    /* 23 */ HRESULT (SFCOP_STDCALL *GetLightState)(IDirect3DDevice3 *self, D3DLIGHTSTATETYPE state_type, DWORD *value);
    /* 24 */ HRESULT (SFCOP_STDCALL *SetLightState)(IDirect3DDevice3 *self, D3DLIGHTSTATETYPE state_type, DWORD value);
    /* 25 */ HRESULT (SFCOP_STDCALL *SetTransform)(IDirect3DDevice3 *self, D3DTRANSFORMSTATETYPE state_type, D3DMATRIX *matrix);
    /* 26 */ HRESULT (SFCOP_STDCALL *GetTransform)(IDirect3DDevice3 *self, D3DTRANSFORMSTATETYPE state_type, D3DMATRIX *matrix);
    /* 27 */ HRESULT (SFCOP_STDCALL *MultiplyTransform)(IDirect3DDevice3 *self, D3DTRANSFORMSTATETYPE state_type, D3DMATRIX *matrix);
    /* 28 */ HRESULT (SFCOP_STDCALL *DrawPrimitive)(IDirect3DDevice3 *self, D3DPRIMITIVETYPE primitive_type, DWORD vertex_format, void *vertices, DWORD vertex_count, DWORD flags);
    /* 29 */ HRESULT (SFCOP_STDCALL *DrawIndexedPrimitive)(IDirect3DDevice3 *self, D3DPRIMITIVETYPE primitive_type, DWORD vertex_format, void *vertices, DWORD vertex_count, WORD *indices, DWORD index_count, DWORD flags);
    /* 30 */ HRESULT (SFCOP_STDCALL *SetClipStatus)(IDirect3DDevice3 *self, D3DCLIPSTATUS *clip_status);
    /* 31 */ HRESULT (SFCOP_STDCALL *GetClipStatus)(IDirect3DDevice3 *self, D3DCLIPSTATUS *clip_status);
    /* 32 */ HRESULT (SFCOP_STDCALL *DrawPrimitiveStrided)(IDirect3DDevice3 *self, D3DPRIMITIVETYPE primitive_type, DWORD vertex_format, D3DDRAWPRIMITIVESTRIDEDDATA *strided_data, DWORD vertex_count, DWORD flags);
    /* 33 */ HRESULT (SFCOP_STDCALL *DrawIndexedPrimitiveStrided)(IDirect3DDevice3 *self, D3DPRIMITIVETYPE primitive_type, DWORD vertex_format, D3DDRAWPRIMITIVESTRIDEDDATA *strided_data, DWORD vertex_count, WORD *indices, DWORD index_count, DWORD flags);
    /* 34 */ HRESULT (SFCOP_STDCALL *DrawPrimitiveVB)(IDirect3DDevice3 *self, D3DPRIMITIVETYPE primitive_type, IDirect3DVertexBuffer *buffer, DWORD start_vertex, DWORD vertex_count, DWORD flags);
    /* 35 */ HRESULT (SFCOP_STDCALL *DrawIndexedPrimitiveVB)(IDirect3DDevice3 *self, D3DPRIMITIVETYPE primitive_type, IDirect3DVertexBuffer *buffer, WORD *indices, DWORD index_count, DWORD flags);
    /* 36 */ HRESULT (SFCOP_STDCALL *ComputeSphereVisibility)(IDirect3DDevice3 *self, D3DVECTOR *centers, D3DVALUE *radii, DWORD sphere_count, DWORD flags, DWORD *results);
    /* 37 */ HRESULT (SFCOP_STDCALL *GetTexture)(IDirect3DDevice3 *self, DWORD stage, IDirect3DTexture2 **texture);
    /* 38 */ HRESULT (SFCOP_STDCALL *SetTexture)(IDirect3DDevice3 *self, DWORD stage, IDirect3DTexture2 *texture);
    /* 39 */ HRESULT (SFCOP_STDCALL *GetTextureStageState)(IDirect3DDevice3 *self, DWORD stage, D3DTEXTURESTAGESTATETYPE state_type, DWORD *value);
    /* 40 */ HRESULT (SFCOP_STDCALL *SetTextureStageState)(IDirect3DDevice3 *self, DWORD stage, D3DTEXTURESTAGESTATETYPE state_type, DWORD value);
    /* 41 */ HRESULT (SFCOP_STDCALL *ValidateDevice)(IDirect3DDevice3 *self, DWORD *pass_count);
    // clang-format on
} IDirect3DDevice3Vtbl;
struct IDirect3DDevice3 {
    const IDirect3DDevice3Vtbl *lpVtbl;
};
SFCOP_COM_ASSERT_SLOTS(IDirect3DDevice3Vtbl, 42);

SFCOP_COM_END

#endif
