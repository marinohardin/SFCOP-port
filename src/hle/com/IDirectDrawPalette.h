/* IDirectDrawPalette: method order and IID from the public header include/ddraw.h of Wine 9.0.
 * Written for this project in its own form; obtained from IDirectDraw4::CreatePalette. */
#ifndef SFCOP_HLE_COM_IDIRECTDRAWPALETTE_H
#define SFCOP_HLE_COM_IDIRECTDRAWPALETTE_H

#include "DDrawTypes.h"

SFCOP_COM_BEGIN

/* {6C14DB84-A733-11CE-A521-0020AF0BE560} */
static const GUID kIID_IDirectDrawPalette = {0x6C14DB84, 0xA733, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};

typedef struct IDirectDrawPaletteVtbl {
    // clang-format off
    /*  0 */ HRESULT (SFCOP_STDCALL *QueryInterface)(IDirectDrawPalette *self, REFIID riid, void **object);
    /*  1 */ ULONG (SFCOP_STDCALL *AddRef)(IDirectDrawPalette *self);
    /*  2 */ ULONG (SFCOP_STDCALL *Release)(IDirectDrawPalette *self);
    /*  3 */ HRESULT (SFCOP_STDCALL *GetCaps)(IDirectDrawPalette *self, DWORD *caps);
    /*  4 */ HRESULT (SFCOP_STDCALL *GetEntries)(IDirectDrawPalette *self, DWORD flags, DWORD base, DWORD count, PALETTEENTRY *entries);
    /*  5 */ HRESULT (SFCOP_STDCALL *Initialize)(IDirectDrawPalette *self, IDirectDraw *ddraw, DWORD flags, PALETTEENTRY *color_table);
    /*  6 */ HRESULT (SFCOP_STDCALL *SetEntries)(IDirectDrawPalette *self, DWORD flags, DWORD start, DWORD count, PALETTEENTRY *entries);
    // clang-format on
} IDirectDrawPaletteVtbl;
struct IDirectDrawPalette {
    const IDirectDrawPaletteVtbl *lpVtbl;
};
SFCOP_COM_ASSERT_SLOTS(IDirectDrawPaletteVtbl, 7);

SFCOP_COM_END

#endif
