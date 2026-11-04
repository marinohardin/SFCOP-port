/* IDirectDraw: method order and IID from the public header include/ddraw.h of Wine 9.0.
 * Written for this project in its own form; obtained from DirectDrawCreate. */
#ifndef SFCOP_HLE_COM_IDIRECTDRAW_H
#define SFCOP_HLE_COM_IDIRECTDRAW_H

#include "DDrawTypes.h"

SFCOP_COM_BEGIN

/* {6C14DB80-A733-11CE-A521-0020AF0BE560} */
static const GUID kIID_IDirectDraw = {0x6C14DB80, 0xA733, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};

typedef struct IDirectDrawVtbl {
    // clang-format off
    /*  0 */ HRESULT (SFCOP_STDCALL *QueryInterface)(IDirectDraw *self, REFIID riid, void **object);
    /*  1 */ ULONG (SFCOP_STDCALL *AddRef)(IDirectDraw *self);
    /*  2 */ ULONG (SFCOP_STDCALL *Release)(IDirectDraw *self);
    /*  3 */ HRESULT (SFCOP_STDCALL *Compact)(IDirectDraw *self);
    /*  4 */ HRESULT (SFCOP_STDCALL *CreateClipper)(IDirectDraw *self, DWORD flags, IDirectDrawClipper **clipper, IUnknown *outer);
    /*  5 */ HRESULT (SFCOP_STDCALL *CreatePalette)(IDirectDraw *self, DWORD flags, PALETTEENTRY *color_table, IDirectDrawPalette **palette, IUnknown *outer);
    /*  6 */ HRESULT (SFCOP_STDCALL *CreateSurface)(IDirectDraw *self, DDSURFACEDESC *surface_desc, IDirectDrawSurface **surface, IUnknown *outer);
    /*  7 */ HRESULT (SFCOP_STDCALL *DuplicateSurface)(IDirectDraw *self, IDirectDrawSurface *source, IDirectDrawSurface **duplicate);
    /*  8 */ HRESULT (SFCOP_STDCALL *EnumDisplayModes)(IDirectDraw *self, DWORD flags, DDSURFACEDESC *surface_desc, void *context, LPDDENUMMODESCALLBACK callback);
    /*  9 */ HRESULT (SFCOP_STDCALL *EnumSurfaces)(IDirectDraw *self, DWORD flags, DDSURFACEDESC *surface_desc, void *context, LPDDENUMSURFACESCALLBACK callback);
    /* 10 */ HRESULT (SFCOP_STDCALL *FlipToGDISurface)(IDirectDraw *self);
    /* 11 */ HRESULT (SFCOP_STDCALL *GetCaps)(IDirectDraw *self, DDCAPS *driver_caps, DDCAPS *emulation_caps);
    /* 12 */ HRESULT (SFCOP_STDCALL *GetDisplayMode)(IDirectDraw *self, DDSURFACEDESC *surface_desc);
    /* 13 */ HRESULT (SFCOP_STDCALL *GetFourCCCodes)(IDirectDraw *self, DWORD *count, DWORD *codes);
    /* 14 */ HRESULT (SFCOP_STDCALL *GetGDISurface)(IDirectDraw *self, IDirectDrawSurface **surface);
    /* 15 */ HRESULT (SFCOP_STDCALL *GetMonitorFrequency)(IDirectDraw *self, DWORD *frequency);
    /* 16 */ HRESULT (SFCOP_STDCALL *GetScanLine)(IDirectDraw *self, DWORD *scan_line);
    /* 17 */ HRESULT (SFCOP_STDCALL *GetVerticalBlankStatus)(IDirectDraw *self, BOOL *in_vblank);
    /* 18 */ HRESULT (SFCOP_STDCALL *Initialize)(IDirectDraw *self, GUID *driver);
    /* 19 */ HRESULT (SFCOP_STDCALL *RestoreDisplayMode)(IDirectDraw *self);
    /* 20 */ HRESULT (SFCOP_STDCALL *SetCooperativeLevel)(IDirectDraw *self, HWND window, DWORD flags);
    /* 21 */ HRESULT (SFCOP_STDCALL *SetDisplayMode)(IDirectDraw *self, DWORD width, DWORD height, DWORD bits_per_pixel);
    /* 22 */ HRESULT (SFCOP_STDCALL *WaitForVerticalBlank)(IDirectDraw *self, DWORD flags, HANDLE event);
    // clang-format on
} IDirectDrawVtbl;
struct IDirectDraw {
    const IDirectDrawVtbl *lpVtbl;
};
SFCOP_COM_ASSERT_SLOTS(IDirectDrawVtbl, 23);

SFCOP_COM_END

#endif
