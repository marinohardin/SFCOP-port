/* IDirectInputA: method order and IID from the public header include/dinput.h of Wine 9.0.
 * Written for this project in its own form; obtained from DirectInputCreateA. */
#ifndef SFCOP_HLE_COM_IDIRECTINPUTA_H
#define SFCOP_HLE_COM_IDIRECTINPUTA_H

#include "DInputTypes.h"

SFCOP_COM_BEGIN

/* {89521360-AA8A-11CF-BFC7-444553540000} */
static const GUID kIID_IDirectInputA = {0x89521360, 0xAA8A, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

typedef struct IDirectInputAVtbl {
    // clang-format off
    /*  0 */ HRESULT (SFCOP_STDCALL *QueryInterface)(IDirectInputA *self, REFIID riid, void **object);
    /*  1 */ ULONG (SFCOP_STDCALL *AddRef)(IDirectInputA *self);
    /*  2 */ ULONG (SFCOP_STDCALL *Release)(IDirectInputA *self);
    /*  3 */ HRESULT (SFCOP_STDCALL *CreateDevice)(IDirectInputA *self, REFGUID device_guid, IDirectInputDeviceA **device, IUnknown *outer);
    /*  4 */ HRESULT (SFCOP_STDCALL *EnumDevices)(IDirectInputA *self, DWORD device_type, LPDIENUMDEVICESCALLBACKA callback, void *context, DWORD flags);
    /*  5 */ HRESULT (SFCOP_STDCALL *GetDeviceStatus)(IDirectInputA *self, REFGUID device_guid);
    /*  6 */ HRESULT (SFCOP_STDCALL *RunControlPanel)(IDirectInputA *self, HWND owner, DWORD flags);
    /*  7 */ HRESULT (SFCOP_STDCALL *Initialize)(IDirectInputA *self, HINSTANCE instance, DWORD version);
    // clang-format on
} IDirectInputAVtbl;
struct IDirectInputA {
    const IDirectInputAVtbl *lpVtbl;
};
SFCOP_COM_ASSERT_SLOTS(IDirectInputAVtbl, 8);

SFCOP_COM_END

#endif
