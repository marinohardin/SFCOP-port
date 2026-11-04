/* IDirectDrawSurface4: method order and IID from the public header include/ddraw.h of Wine 9.0.
 * Written for this project in its own form; obtained from IDirectDraw4::CreateSurface. */
#ifndef SFCOP_HLE_COM_IDIRECTDRAWSURFACE4_H
#define SFCOP_HLE_COM_IDIRECTDRAWSURFACE4_H

#include "DDrawTypes.h"

SFCOP_COM_BEGIN

/* {0B2B8630-AD35-11D0-8EA6-00609797EA5B} */
static const GUID kIID_IDirectDrawSurface4 = {0x0B2B8630, 0xAD35, 0x11D0, {0x8E, 0xA6, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B}};

typedef struct IDirectDrawSurface4Vtbl {
    // clang-format off
    /*  0 */ HRESULT (SFCOP_STDCALL *QueryInterface)(IDirectDrawSurface4 *self, REFIID riid, void **object);
    /*  1 */ ULONG (SFCOP_STDCALL *AddRef)(IDirectDrawSurface4 *self);
    /*  2 */ ULONG (SFCOP_STDCALL *Release)(IDirectDrawSurface4 *self);
    /*  3 */ HRESULT (SFCOP_STDCALL *AddAttachedSurface)(IDirectDrawSurface4 *self, IDirectDrawSurface4 *attachment);
    /*  4 */ HRESULT (SFCOP_STDCALL *AddOverlayDirtyRect)(IDirectDrawSurface4 *self, RECT *rect);
    /*  5 */ HRESULT (SFCOP_STDCALL *Blt)(IDirectDrawSurface4 *self, RECT *dest_rect, IDirectDrawSurface4 *source, RECT *source_rect, DWORD flags, DDBLTFX *blt_fx);
    /*  6 */ HRESULT (SFCOP_STDCALL *BltBatch)(IDirectDrawSurface4 *self, DDBLTBATCH *batch, DWORD count, DWORD flags);
    /*  7 */ HRESULT (SFCOP_STDCALL *BltFast)(IDirectDrawSurface4 *self, DWORD x, DWORD y, IDirectDrawSurface4 *source, RECT *source_rect, DWORD flags);
    /*  8 */ HRESULT (SFCOP_STDCALL *DeleteAttachedSurface)(IDirectDrawSurface4 *self, DWORD flags, IDirectDrawSurface4 *attachment);
    /*  9 */ HRESULT (SFCOP_STDCALL *EnumAttachedSurfaces)(IDirectDrawSurface4 *self, void *context, LPDDENUMSURFACESCALLBACK2 callback);
    /* 10 */ HRESULT (SFCOP_STDCALL *EnumOverlayZOrders)(IDirectDrawSurface4 *self, DWORD flags, void *context, LPDDENUMSURFACESCALLBACK2 callback);
    /* 11 */ HRESULT (SFCOP_STDCALL *Flip)(IDirectDrawSurface4 *self, IDirectDrawSurface4 *target, DWORD flags);
    /* 12 */ HRESULT (SFCOP_STDCALL *GetAttachedSurface)(IDirectDrawSurface4 *self, DDSCAPS2 *caps, IDirectDrawSurface4 **attachment);
    /* 13 */ HRESULT (SFCOP_STDCALL *GetBltStatus)(IDirectDrawSurface4 *self, DWORD flags);
    /* 14 */ HRESULT (SFCOP_STDCALL *GetCaps)(IDirectDrawSurface4 *self, DDSCAPS2 *caps);
    /* 15 */ HRESULT (SFCOP_STDCALL *GetClipper)(IDirectDrawSurface4 *self, IDirectDrawClipper **clipper);
    /* 16 */ HRESULT (SFCOP_STDCALL *GetColorKey)(IDirectDrawSurface4 *self, DWORD flags, DDCOLORKEY *color_key);
    /* 17 */ HRESULT (SFCOP_STDCALL *GetDC)(IDirectDrawSurface4 *self, HDC *dc);
    /* 18 */ HRESULT (SFCOP_STDCALL *GetFlipStatus)(IDirectDrawSurface4 *self, DWORD flags);
    /* 19 */ HRESULT (SFCOP_STDCALL *GetOverlayPosition)(IDirectDrawSurface4 *self, LONG *x, LONG *y);
    /* 20 */ HRESULT (SFCOP_STDCALL *GetPalette)(IDirectDrawSurface4 *self, IDirectDrawPalette **palette);
    /* 21 */ HRESULT (SFCOP_STDCALL *GetPixelFormat)(IDirectDrawSurface4 *self, DDPIXELFORMAT *pixel_format);
    /* 22 */ HRESULT (SFCOP_STDCALL *GetSurfaceDesc)(IDirectDrawSurface4 *self, DDSURFACEDESC2 *surface_desc);
    /* 23 */ HRESULT (SFCOP_STDCALL *Initialize)(IDirectDrawSurface4 *self, IDirectDraw *ddraw, DDSURFACEDESC2 *surface_desc);
    /* 24 */ HRESULT (SFCOP_STDCALL *IsLost)(IDirectDrawSurface4 *self);
    /* 25 */ HRESULT (SFCOP_STDCALL *Lock)(IDirectDrawSurface4 *self, RECT *rect, DDSURFACEDESC2 *surface_desc, DWORD flags, HANDLE event);
    /* 26 */ HRESULT (SFCOP_STDCALL *ReleaseDC)(IDirectDrawSurface4 *self, HDC dc);
    /* 27 */ HRESULT (SFCOP_STDCALL *Restore)(IDirectDrawSurface4 *self);
    /* 28 */ HRESULT (SFCOP_STDCALL *SetClipper)(IDirectDrawSurface4 *self, IDirectDrawClipper *clipper);
    /* 29 */ HRESULT (SFCOP_STDCALL *SetColorKey)(IDirectDrawSurface4 *self, DWORD flags, DDCOLORKEY *color_key);
    /* 30 */ HRESULT (SFCOP_STDCALL *SetOverlayPosition)(IDirectDrawSurface4 *self, LONG x, LONG y);
    /* 31 */ HRESULT (SFCOP_STDCALL *SetPalette)(IDirectDrawSurface4 *self, IDirectDrawPalette *palette);
    /* 32 */ HRESULT (SFCOP_STDCALL *Unlock)(IDirectDrawSurface4 *self, RECT *rect);
    /* 33 */ HRESULT (SFCOP_STDCALL *UpdateOverlay)(IDirectDrawSurface4 *self, RECT *source_rect, IDirectDrawSurface4 *destination, RECT *dest_rect, DWORD flags, DDOVERLAYFX *overlay_fx);
    /* 34 */ HRESULT (SFCOP_STDCALL *UpdateOverlayDisplay)(IDirectDrawSurface4 *self, DWORD flags);
    /* 35 */ HRESULT (SFCOP_STDCALL *UpdateOverlayZOrder)(IDirectDrawSurface4 *self, DWORD flags, IDirectDrawSurface4 *reference);
    /* 36 */ HRESULT (SFCOP_STDCALL *GetDDInterface)(IDirectDrawSurface4 *self, void **ddraw);
    /* 37 */ HRESULT (SFCOP_STDCALL *PageLock)(IDirectDrawSurface4 *self, DWORD flags);
    /* 38 */ HRESULT (SFCOP_STDCALL *PageUnlock)(IDirectDrawSurface4 *self, DWORD flags);
    /* 39 */ HRESULT (SFCOP_STDCALL *SetSurfaceDesc)(IDirectDrawSurface4 *self, DDSURFACEDESC2 *surface_desc, DWORD flags);
    /* 40 */ HRESULT (SFCOP_STDCALL *SetPrivateData)(IDirectDrawSurface4 *self, REFGUID tag, void *data, DWORD size, DWORD flags);
    /* 41 */ HRESULT (SFCOP_STDCALL *GetPrivateData)(IDirectDrawSurface4 *self, REFGUID tag, void *data, DWORD *size);
    /* 42 */ HRESULT (SFCOP_STDCALL *FreePrivateData)(IDirectDrawSurface4 *self, REFGUID tag);
    /* 43 */ HRESULT (SFCOP_STDCALL *GetUniquenessValue)(IDirectDrawSurface4 *self, DWORD *value);
    /* 44 */ HRESULT (SFCOP_STDCALL *ChangeUniquenessValue)(IDirectDrawSurface4 *self);
    // clang-format on
} IDirectDrawSurface4Vtbl;
struct IDirectDrawSurface4 {
    const IDirectDrawSurface4Vtbl *lpVtbl;
};
SFCOP_COM_ASSERT_SLOTS(IDirectDrawSurface4Vtbl, 45);

SFCOP_COM_END

#endif
