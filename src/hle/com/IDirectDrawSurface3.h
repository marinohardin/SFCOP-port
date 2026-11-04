/* IDirectDrawSurface3: method order and IID from the public header include/ddraw.h of Wine 9.0.
 * Written for this project in its own form; obtained from IDirectDrawSurface::QueryInterface. */
#ifndef SFCOP_HLE_COM_IDIRECTDRAWSURFACE3_H
#define SFCOP_HLE_COM_IDIRECTDRAWSURFACE3_H

#include "DDrawTypes.h"

SFCOP_COM_BEGIN

/* {DA044E00-69B2-11D0-A1D5-00AA00B8DFBB} */
static const GUID kIID_IDirectDrawSurface3 = {0xDA044E00, 0x69B2, 0x11D0, {0xA1, 0xD5, 0x00, 0xAA, 0x00, 0xB8, 0xDF, 0xBB}};

typedef struct IDirectDrawSurface3Vtbl {
    // clang-format off
    /*  0 */ HRESULT (SFCOP_STDCALL *QueryInterface)(IDirectDrawSurface3 *self, REFIID riid, void **object);
    /*  1 */ ULONG (SFCOP_STDCALL *AddRef)(IDirectDrawSurface3 *self);
    /*  2 */ ULONG (SFCOP_STDCALL *Release)(IDirectDrawSurface3 *self);
    /*  3 */ HRESULT (SFCOP_STDCALL *AddAttachedSurface)(IDirectDrawSurface3 *self, IDirectDrawSurface3 *attachment);
    /*  4 */ HRESULT (SFCOP_STDCALL *AddOverlayDirtyRect)(IDirectDrawSurface3 *self, RECT *rect);
    /*  5 */ HRESULT (SFCOP_STDCALL *Blt)(IDirectDrawSurface3 *self, RECT *dest_rect, IDirectDrawSurface3 *source, RECT *source_rect, DWORD flags, DDBLTFX *blt_fx);
    /*  6 */ HRESULT (SFCOP_STDCALL *BltBatch)(IDirectDrawSurface3 *self, DDBLTBATCH *batch, DWORD count, DWORD flags);
    /*  7 */ HRESULT (SFCOP_STDCALL *BltFast)(IDirectDrawSurface3 *self, DWORD x, DWORD y, IDirectDrawSurface3 *source, RECT *source_rect, DWORD flags);
    /*  8 */ HRESULT (SFCOP_STDCALL *DeleteAttachedSurface)(IDirectDrawSurface3 *self, DWORD flags, IDirectDrawSurface3 *attachment);
    /*  9 */ HRESULT (SFCOP_STDCALL *EnumAttachedSurfaces)(IDirectDrawSurface3 *self, void *context, LPDDENUMSURFACESCALLBACK callback);
    /* 10 */ HRESULT (SFCOP_STDCALL *EnumOverlayZOrders)(IDirectDrawSurface3 *self, DWORD flags, void *context, LPDDENUMSURFACESCALLBACK callback);
    /* 11 */ HRESULT (SFCOP_STDCALL *Flip)(IDirectDrawSurface3 *self, IDirectDrawSurface3 *target, DWORD flags);
    /* 12 */ HRESULT (SFCOP_STDCALL *GetAttachedSurface)(IDirectDrawSurface3 *self, DDSCAPS *caps, IDirectDrawSurface3 **attachment);
    /* 13 */ HRESULT (SFCOP_STDCALL *GetBltStatus)(IDirectDrawSurface3 *self, DWORD flags);
    /* 14 */ HRESULT (SFCOP_STDCALL *GetCaps)(IDirectDrawSurface3 *self, DDSCAPS *caps);
    /* 15 */ HRESULT (SFCOP_STDCALL *GetClipper)(IDirectDrawSurface3 *self, IDirectDrawClipper **clipper);
    /* 16 */ HRESULT (SFCOP_STDCALL *GetColorKey)(IDirectDrawSurface3 *self, DWORD flags, DDCOLORKEY *color_key);
    /* 17 */ HRESULT (SFCOP_STDCALL *GetDC)(IDirectDrawSurface3 *self, HDC *dc);
    /* 18 */ HRESULT (SFCOP_STDCALL *GetFlipStatus)(IDirectDrawSurface3 *self, DWORD flags);
    /* 19 */ HRESULT (SFCOP_STDCALL *GetOverlayPosition)(IDirectDrawSurface3 *self, LONG *x, LONG *y);
    /* 20 */ HRESULT (SFCOP_STDCALL *GetPalette)(IDirectDrawSurface3 *self, IDirectDrawPalette **palette);
    /* 21 */ HRESULT (SFCOP_STDCALL *GetPixelFormat)(IDirectDrawSurface3 *self, DDPIXELFORMAT *pixel_format);
    /* 22 */ HRESULT (SFCOP_STDCALL *GetSurfaceDesc)(IDirectDrawSurface3 *self, DDSURFACEDESC *surface_desc);
    /* 23 */ HRESULT (SFCOP_STDCALL *Initialize)(IDirectDrawSurface3 *self, IDirectDraw *ddraw, DDSURFACEDESC *surface_desc);
    /* 24 */ HRESULT (SFCOP_STDCALL *IsLost)(IDirectDrawSurface3 *self);
    /* 25 */ HRESULT (SFCOP_STDCALL *Lock)(IDirectDrawSurface3 *self, RECT *rect, DDSURFACEDESC *surface_desc, DWORD flags, HANDLE event);
    /* 26 */ HRESULT (SFCOP_STDCALL *ReleaseDC)(IDirectDrawSurface3 *self, HDC dc);
    /* 27 */ HRESULT (SFCOP_STDCALL *Restore)(IDirectDrawSurface3 *self);
    /* 28 */ HRESULT (SFCOP_STDCALL *SetClipper)(IDirectDrawSurface3 *self, IDirectDrawClipper *clipper);
    /* 29 */ HRESULT (SFCOP_STDCALL *SetColorKey)(IDirectDrawSurface3 *self, DWORD flags, DDCOLORKEY *color_key);
    /* 30 */ HRESULT (SFCOP_STDCALL *SetOverlayPosition)(IDirectDrawSurface3 *self, LONG x, LONG y);
    /* 31 */ HRESULT (SFCOP_STDCALL *SetPalette)(IDirectDrawSurface3 *self, IDirectDrawPalette *palette);
    /* 32 */ HRESULT (SFCOP_STDCALL *Unlock)(IDirectDrawSurface3 *self, void *data);
    /* 33 */ HRESULT (SFCOP_STDCALL *UpdateOverlay)(IDirectDrawSurface3 *self, RECT *source_rect, IDirectDrawSurface3 *destination, RECT *dest_rect, DWORD flags, DDOVERLAYFX *overlay_fx);
    /* 34 */ HRESULT (SFCOP_STDCALL *UpdateOverlayDisplay)(IDirectDrawSurface3 *self, DWORD flags);
    /* 35 */ HRESULT (SFCOP_STDCALL *UpdateOverlayZOrder)(IDirectDrawSurface3 *self, DWORD flags, IDirectDrawSurface3 *reference);
    /* 36 */ HRESULT (SFCOP_STDCALL *GetDDInterface)(IDirectDrawSurface3 *self, void **ddraw);
    /* 37 */ HRESULT (SFCOP_STDCALL *PageLock)(IDirectDrawSurface3 *self, DWORD flags);
    /* 38 */ HRESULT (SFCOP_STDCALL *PageUnlock)(IDirectDrawSurface3 *self, DWORD flags);
    /* 39 */ HRESULT (SFCOP_STDCALL *SetSurfaceDesc)(IDirectDrawSurface3 *self, DDSURFACEDESC *surface_desc, DWORD flags);
    // clang-format on
} IDirectDrawSurface3Vtbl;
struct IDirectDrawSurface3 {
    const IDirectDrawSurface3Vtbl *lpVtbl;
};
SFCOP_COM_ASSERT_SLOTS(IDirectDrawSurface3Vtbl, 40);

SFCOP_COM_END

#endif
