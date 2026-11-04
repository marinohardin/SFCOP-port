/* IDirectInputDeviceA: method order and IID from the public header include/dinput.h of Wine 9.0.
 * Written for this project in its own form; obtained from IDirectInputA::CreateDevice. */
#ifndef SFCOP_HLE_COM_IDIRECTINPUTDEVICEA_H
#define SFCOP_HLE_COM_IDIRECTINPUTDEVICEA_H

#include "DInputTypes.h"

SFCOP_COM_BEGIN

/* {5944E680-C92E-11CF-BFC7-444553540000} */
static const GUID kIID_IDirectInputDeviceA = {0x5944E680, 0xC92E, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

typedef struct IDirectInputDeviceAVtbl {
    // clang-format off
    /*  0 */ HRESULT (SFCOP_STDCALL *QueryInterface)(IDirectInputDeviceA *self, REFIID riid, void **object);
    /*  1 */ ULONG (SFCOP_STDCALL *AddRef)(IDirectInputDeviceA *self);
    /*  2 */ ULONG (SFCOP_STDCALL *Release)(IDirectInputDeviceA *self);
    /*  3 */ HRESULT (SFCOP_STDCALL *GetCapabilities)(IDirectInputDeviceA *self, DIDEVCAPS *caps);
    /*  4 */ HRESULT (SFCOP_STDCALL *EnumObjects)(IDirectInputDeviceA *self, LPDIENUMDEVICEOBJECTSCALLBACKA callback, void *context, DWORD flags);
    /*  5 */ HRESULT (SFCOP_STDCALL *GetProperty)(IDirectInputDeviceA *self, REFGUID property_guid, DIPROPHEADER *header);
    /*  6 */ HRESULT (SFCOP_STDCALL *SetProperty)(IDirectInputDeviceA *self, REFGUID property_guid, const DIPROPHEADER *header);
    /*  7 */ HRESULT (SFCOP_STDCALL *Acquire)(IDirectInputDeviceA *self);
    /*  8 */ HRESULT (SFCOP_STDCALL *Unacquire)(IDirectInputDeviceA *self);
    /*  9 */ HRESULT (SFCOP_STDCALL *GetDeviceState)(IDirectInputDeviceA *self, DWORD size, void *data);
    /* 10 */ HRESULT (SFCOP_STDCALL *GetDeviceData)(IDirectInputDeviceA *self, DWORD element_size, DIDEVICEOBJECTDATA *data, DWORD *count, DWORD flags);
    /* 11 */ HRESULT (SFCOP_STDCALL *SetDataFormat)(IDirectInputDeviceA *self, const DIDATAFORMAT *format);
    /* 12 */ HRESULT (SFCOP_STDCALL *SetEventNotification)(IDirectInputDeviceA *self, HANDLE event);
    /* 13 */ HRESULT (SFCOP_STDCALL *SetCooperativeLevel)(IDirectInputDeviceA *self, HWND window, DWORD flags);
    /* 14 */ HRESULT (SFCOP_STDCALL *GetObjectInfo)(IDirectInputDeviceA *self, DIDEVICEOBJECTINSTANCEA *object_info, DWORD object, DWORD how);
    /* 15 */ HRESULT (SFCOP_STDCALL *GetDeviceInfo)(IDirectInputDeviceA *self, DIDEVICEINSTANCEA *device_info);
    /* 16 */ HRESULT (SFCOP_STDCALL *RunControlPanel)(IDirectInputDeviceA *self, HWND owner, DWORD flags);
    /* 17 */ HRESULT (SFCOP_STDCALL *Initialize)(IDirectInputDeviceA *self, HINSTANCE instance, DWORD version, REFGUID device_guid);
    // clang-format on
} IDirectInputDeviceAVtbl;
struct IDirectInputDeviceA {
    const IDirectInputDeviceAVtbl *lpVtbl;
};
SFCOP_COM_ASSERT_SLOTS(IDirectInputDeviceAVtbl, 18);

SFCOP_COM_END

#endif
