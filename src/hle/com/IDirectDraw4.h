/* IDirectDraw4: method order and IID from the public header include/ddraw.h of Wine 9.0.
 * Written for this project in its own form; obtained from IDirectDraw::QueryInterface. */
#ifndef SFCOP_HLE_COM_IDIRECTDRAW4_H
#define SFCOP_HLE_COM_IDIRECTDRAW4_H

#include "DDrawTypes.h"

SFCOP_COM_BEGIN

/* {9C59509A-39BD-11D1-8C4A-00C04FD930C5} */
static const GUID kIID_IDirectDraw4 = {0x9C59509A, 0x39BD, 0x11D1, {0x8C, 0x4A, 0x00, 0xC0, 0x4F, 0xD9, 0x30, 0xC5}};

typedef struct IDirectDraw4Vtbl {
    // clang-format off
    /*  0 */ HRESULT (SFCOP_STDCALL *QueryInterface)(IDirectDraw4 *self, REFIID riid, void **object);
    /*  1 */ ULONG (SFCOP_STDCALL *AddRef)(IDirectDraw4 *self);
    /*  2 */ ULONG (SFCOP_STDCALL *Release)(IDirectDraw4 *self);
    /*  3 */ HRESULT (SFCOP_STDCALL *Compact)(IDirectDraw4 *self);
    /*  4 */ HRESULT (SFCOP_STDCALL *CreateClipper)(IDirectDraw4 *self, DWORD flags, IDirectDrawClipper **clipper, IUnknown *outer);
    /*  5 */ HRESULT (SFCOP_STDCALL *CreatePalette)(IDirectDraw4 *self, DWORD flags, PALETTEENTRY *color_table, IDirectDrawPalette **palette, IUnknown *outer);
    /*  6 */ HRESULT (SFCOP_STDCALL *CreateSurface)(IDirectDraw4 *self, DDSURFACEDESC2 *surface_desc, IDirectDrawSurface4 **surface, IUnknown *outer);
    /*  7 */ HRESULT (SFCOP_STDCALL *DuplicateSurface)(IDirectDraw4 *self, IDirectDrawSurface4 *source, IDirectDrawSurface4 **duplicate);
    /*  8 */ HRESULT (SFCOP_STDCALL *EnumDisplayModes)(IDirectDraw4 *self, DWORD flags, DDSURFACEDESC2 *surface_desc, void *context, LPDDENUMMODESCALLBACK2 callback);
    /*  9 */ HRESULT (SFCOP_STDCALL *EnumSurfaces)(IDirectDraw4 *self, DWORD flags, DDSURFACEDESC2 *surface_desc, void *context, LPDDENUMSURFACESCALLBACK2 callback);
    /* 10 */ HRESULT (SFCOP_STDCALL *FlipToGDISurface)(IDirectDraw4 *self);
    /* 11 */ HRESULT (SFCOP_STDCALL *GetCaps)(IDirectDraw4 *self, DDCAPS *driver_caps, DDCAPS *emulation_caps);
    /* 12 */ HRESULT (SFCOP_STDCALL *GetDisplayMode)(IDirectDraw4 *self, DDSURFACEDESC2 *surface_desc);
    /* 13 */ HRESULT (SFCOP_STDCALL *GetFourCCCodes)(IDirectDraw4 *self, DWORD *count, DWORD *codes);
    /* 14 */ HRESULT (SFCOP_STDCALL *GetGDISurface)(IDirectDraw4 *self, IDirectDrawSurface4 **surface);
    /* 15 */ HRESULT (SFCOP_STDCALL *GetMonitorFrequency)(IDirectDraw4 *self, DWORD *frequency);
    /* 16 */ HRESULT (SFCOP_STDCALL *GetScanLine)(IDirectDraw4 *self, DWORD *scan_line);
    /* 17 */ HRESULT (SFCOP_STDCALL *GetVerticalBlankStatus)(IDirectDraw4 *self, BOOL *in_vblank);
    /* 18 */ HRESULT (SFCOP_STDCALL *Initialize)(IDirectDraw4 *self, GUID *driver);
    /* 19 */ HRESULT (SFCOP_STDCALL *RestoreDisplayMode)(IDirectDraw4 *self);
    /* 20 */ HRESULT (SFCOP_STDCALL *SetCooperativeLevel)(IDirectDraw4 *self, HWND window, DWORD flags);
    /* 21 */ HRESULT (SFCOP_STDCALL *SetDisplayMode)(IDirectDraw4 *self, DWORD width, DWORD height, DWORD bits_per_pixel, DWORD refresh_rate, DWORD flags);
    /* 22 */ HRESULT (SFCOP_STDCALL *WaitForVerticalBlank)(IDirectDraw4 *self, DWORD flags, HANDLE event);
    /* 23 */ HRESULT (SFCOP_STDCALL *GetAvailableVidMem)(IDirectDraw4 *self, DDSCAPS2 *caps, DWORD *total, DWORD *available);
    /* 24 */ HRESULT (SFCOP_STDCALL *GetSurfaceFromDC)(IDirectDraw4 *self, HDC dc, IDirectDrawSurface4 **surface);
    /* 25 */ HRESULT (SFCOP_STDCALL *RestoreAllSurfaces)(IDirectDraw4 *self);
    /* 26 */ HRESULT (SFCOP_STDCALL *TestCooperativeLevel)(IDirectDraw4 *self);
    /* 27 */ HRESULT (SFCOP_STDCALL *GetDeviceIdentifier)(IDirectDraw4 *self, DDDEVICEIDENTIFIER *identifier, DWORD flags);
    // clang-format on
} IDirectDraw4Vtbl;
struct IDirectDraw4 {
    const IDirectDraw4Vtbl *lpVtbl;
};
SFCOP_COM_ASSERT_SLOTS(IDirectDraw4Vtbl, 28);

SFCOP_COM_END

#endif
