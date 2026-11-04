/* IDirectDraw2: method order and IID from the public header include/ddraw.h of Wine 9.0.
 * Written for this project in its own form; obtained from IDirectDraw::QueryInterface. */
#ifndef SFCOP_HLE_COM_IDIRECTDRAW2_H
#define SFCOP_HLE_COM_IDIRECTDRAW2_H

#include "DDrawTypes.h"

SFCOP_COM_BEGIN

/* {B3A6F3E0-2B43-11CF-A2DE-00AA00B93356} */
static const GUID kIID_IDirectDraw2 = {0xB3A6F3E0, 0x2B43, 0x11CF, {0xA2, 0xDE, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56}};

typedef struct IDirectDraw2Vtbl {
    // clang-format off
    /*  0 */ HRESULT (SFCOP_STDCALL *QueryInterface)(IDirectDraw2 *self, REFIID riid, void **object);
    /*  1 */ ULONG (SFCOP_STDCALL *AddRef)(IDirectDraw2 *self);
    /*  2 */ ULONG (SFCOP_STDCALL *Release)(IDirectDraw2 *self);
    /*  3 */ HRESULT (SFCOP_STDCALL *Compact)(IDirectDraw2 *self);
    /*  4 */ HRESULT (SFCOP_STDCALL *CreateClipper)(IDirectDraw2 *self, DWORD flags, IDirectDrawClipper **clipper, IUnknown *outer);
    /*  5 */ HRESULT (SFCOP_STDCALL *CreatePalette)(IDirectDraw2 *self, DWORD flags, PALETTEENTRY *color_table, IDirectDrawPalette **palette, IUnknown *outer);
    /*  6 */ HRESULT (SFCOP_STDCALL *CreateSurface)(IDirectDraw2 *self, DDSURFACEDESC *surface_desc, IDirectDrawSurface **surface, IUnknown *outer);
    /*  7 */ HRESULT (SFCOP_STDCALL *DuplicateSurface)(IDirectDraw2 *self, IDirectDrawSurface *source, IDirectDrawSurface **duplicate);
    /*  8 */ HRESULT (SFCOP_STDCALL *EnumDisplayModes)(IDirectDraw2 *self, DWORD flags, DDSURFACEDESC *surface_desc, void *context, LPDDENUMMODESCALLBACK callback);
    /*  9 */ HRESULT (SFCOP_STDCALL *EnumSurfaces)(IDirectDraw2 *self, DWORD flags, DDSURFACEDESC *surface_desc, void *context, LPDDENUMSURFACESCALLBACK callback);
    /* 10 */ HRESULT (SFCOP_STDCALL *FlipToGDISurface)(IDirectDraw2 *self);
    /* 11 */ HRESULT (SFCOP_STDCALL *GetCaps)(IDirectDraw2 *self, DDCAPS *driver_caps, DDCAPS *emulation_caps);
    /* 12 */ HRESULT (SFCOP_STDCALL *GetDisplayMode)(IDirectDraw2 *self, DDSURFACEDESC *surface_desc);
    /* 13 */ HRESULT (SFCOP_STDCALL *GetFourCCCodes)(IDirectDraw2 *self, DWORD *count, DWORD *codes);
    /* 14 */ HRESULT (SFCOP_STDCALL *GetGDISurface)(IDirectDraw2 *self, IDirectDrawSurface **surface);
    /* 15 */ HRESULT (SFCOP_STDCALL *GetMonitorFrequency)(IDirectDraw2 *self, DWORD *frequency);
    /* 16 */ HRESULT (SFCOP_STDCALL *GetScanLine)(IDirectDraw2 *self, DWORD *scan_line);
    /* 17 */ HRESULT (SFCOP_STDCALL *GetVerticalBlankStatus)(IDirectDraw2 *self, BOOL *in_vblank);
    /* 18 */ HRESULT (SFCOP_STDCALL *Initialize)(IDirectDraw2 *self, GUID *driver);
    /* 19 */ HRESULT (SFCOP_STDCALL *RestoreDisplayMode)(IDirectDraw2 *self);
    /* 20 */ HRESULT (SFCOP_STDCALL *SetCooperativeLevel)(IDirectDraw2 *self, HWND window, DWORD flags);
    /* 21 */ HRESULT (SFCOP_STDCALL *SetDisplayMode)(IDirectDraw2 *self, DWORD width, DWORD height, DWORD bits_per_pixel, DWORD refresh_rate, DWORD flags);
    /* 22 */ HRESULT (SFCOP_STDCALL *WaitForVerticalBlank)(IDirectDraw2 *self, DWORD flags, HANDLE event);
    /* 23 */ HRESULT (SFCOP_STDCALL *GetAvailableVidMem)(IDirectDraw2 *self, DDSCAPS *caps, DWORD *total, DWORD *available);
    // clang-format on
} IDirectDraw2Vtbl;
struct IDirectDraw2 {
    const IDirectDraw2Vtbl *lpVtbl;
};
SFCOP_COM_ASSERT_SLOTS(IDirectDraw2Vtbl, 24);

SFCOP_COM_END

#endif
