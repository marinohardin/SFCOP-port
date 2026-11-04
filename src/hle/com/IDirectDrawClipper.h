/* IDirectDrawClipper: method order and IID from the public header include/ddraw.h of Wine 9.0.
 * Written for this project in its own form; obtained from IDirectDraw4::CreateClipper and
 * DirectDrawCreateClipper. */
#ifndef SFCOP_HLE_COM_IDIRECTDRAWCLIPPER_H
#define SFCOP_HLE_COM_IDIRECTDRAWCLIPPER_H

#include "DDrawTypes.h"

SFCOP_COM_BEGIN

/* {6C14DB85-A733-11CE-A521-0020AF0BE560} */
static const GUID kIID_IDirectDrawClipper = {0x6C14DB85, 0xA733, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};

typedef struct IDirectDrawClipperVtbl {
    // clang-format off
    /*  0 */ HRESULT (SFCOP_STDCALL *QueryInterface)(IDirectDrawClipper *self, REFIID riid, void **object);
    /*  1 */ ULONG (SFCOP_STDCALL *AddRef)(IDirectDrawClipper *self);
    /*  2 */ ULONG (SFCOP_STDCALL *Release)(IDirectDrawClipper *self);
    /*  3 */ HRESULT (SFCOP_STDCALL *GetClipList)(IDirectDrawClipper *self, RECT *rect, RGNDATA *clip_list, DWORD *size);
    /*  4 */ HRESULT (SFCOP_STDCALL *GetHWnd)(IDirectDrawClipper *self, HWND *window);
    /*  5 */ HRESULT (SFCOP_STDCALL *Initialize)(IDirectDrawClipper *self, IDirectDraw *ddraw, DWORD flags);
    /*  6 */ HRESULT (SFCOP_STDCALL *IsClipListChanged)(IDirectDrawClipper *self, BOOL *changed);
    /*  7 */ HRESULT (SFCOP_STDCALL *SetClipList)(IDirectDrawClipper *self, RGNDATA *clip_list, DWORD flags);
    /*  8 */ HRESULT (SFCOP_STDCALL *SetHWnd)(IDirectDrawClipper *self, DWORD flags, HWND window);
    // clang-format on
} IDirectDrawClipperVtbl;
struct IDirectDrawClipper {
    const IDirectDrawClipperVtbl *lpVtbl;
};
SFCOP_COM_ASSERT_SLOTS(IDirectDrawClipperVtbl, 9);

SFCOP_COM_END

#endif
