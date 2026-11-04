/* IDirectPlayLobby3A: method order and IID from the public header include/dplobby.h of Wine 9.0.
 * Written for this project in its own form; obtained from CoCreateInstance(CLSID_DirectPlayLobby). */
#ifndef SFCOP_HLE_COM_IDIRECTPLAYLOBBY3A_H
#define SFCOP_HLE_COM_IDIRECTPLAYLOBBY3A_H

#include "DPlayTypes.h"

SFCOP_COM_BEGIN

/* {2FE8F810-B2A5-11D0-A787-0000F803ABFC} */
static const GUID kCLSID_DirectPlayLobby = {0x2FE8F810, 0xB2A5, 0x11D0, {0xA7, 0x87, 0x00, 0x00, 0xF8, 0x03, 0xAB, 0xFC}};

/* {2DB72491-652C-11D1-A7A8-0000F803ABFC} */
static const GUID kIID_IDirectPlayLobby3A = {0x2DB72491, 0x652C, 0x11D1, {0xA7, 0xA8, 0x00, 0x00, 0xF8, 0x03, 0xAB, 0xFC}};

typedef struct IDirectPlayLobby3AVtbl {
    // clang-format off
    /*  0 */ HRESULT (SFCOP_STDCALL *QueryInterface)(IDirectPlayLobby3A *self, REFIID riid, void **object);
    /*  1 */ ULONG (SFCOP_STDCALL *AddRef)(IDirectPlayLobby3A *self);
    /*  2 */ ULONG (SFCOP_STDCALL *Release)(IDirectPlayLobby3A *self);
    /*  3 */ HRESULT (SFCOP_STDCALL *Connect)(IDirectPlayLobby3A *self, DWORD flags, void **directplay, IUnknown *outer);
    /*  4 */ HRESULT (SFCOP_STDCALL *CreateAddress)(IDirectPlayLobby3A *self, REFGUID provider, REFGUID address_type, const void *data, DWORD data_size, void *address, DWORD *address_size);
    /*  5 */ HRESULT (SFCOP_STDCALL *EnumAddress)(IDirectPlayLobby3A *self, LPDPENUMADDRESSCALLBACK callback, const void *address, DWORD address_size, void *context);
    /*  6 */ HRESULT (SFCOP_STDCALL *EnumAddressTypes)(IDirectPlayLobby3A *self, LPDPLENUMADDRESSTYPESCALLBACK callback, REFGUID provider, void *context, DWORD flags);
    /*  7 */ HRESULT (SFCOP_STDCALL *EnumLocalApplications)(IDirectPlayLobby3A *self, LPDPLENUMLOCALAPPLICATIONSCALLBACK callback, void *context, DWORD flags);
    /*  8 */ HRESULT (SFCOP_STDCALL *GetConnectionSettings)(IDirectPlayLobby3A *self, DWORD application_id, void *settings, DWORD *settings_size);
    /*  9 */ HRESULT (SFCOP_STDCALL *ReceiveLobbyMessage)(IDirectPlayLobby3A *self, DWORD flags, DWORD application_id, DWORD *message_flags, void *data, DWORD *data_size);
    /* 10 */ HRESULT (SFCOP_STDCALL *RunApplication)(IDirectPlayLobby3A *self, DWORD flags, DWORD *application_id, DPLCONNECTION *connection, HANDLE event);
    /* 11 */ HRESULT (SFCOP_STDCALL *SendLobbyMessage)(IDirectPlayLobby3A *self, DWORD flags, DWORD application_id, void *data, DWORD data_size);
    /* 12 */ HRESULT (SFCOP_STDCALL *SetConnectionSettings)(IDirectPlayLobby3A *self, DWORD flags, DWORD application_id, DPLCONNECTION *connection);
    /* 13 */ HRESULT (SFCOP_STDCALL *SetLobbyMessageEvent)(IDirectPlayLobby3A *self, DWORD flags, DWORD application_id, HANDLE event);
    /* 14 */ HRESULT (SFCOP_STDCALL *CreateCompoundAddress)(IDirectPlayLobby3A *self, const DPCOMPOUNDADDRESSELEMENT *elements, DWORD element_count, void *address, DWORD *address_size);
    /* 15 */ HRESULT (SFCOP_STDCALL *ConnectEx)(IDirectPlayLobby3A *self, DWORD flags, REFIID riid, void **directplay, IUnknown *outer);
    /* 16 */ HRESULT (SFCOP_STDCALL *RegisterApplication)(IDirectPlayLobby3A *self, DWORD flags, DPAPPLICATIONDESC *application);
    /* 17 */ HRESULT (SFCOP_STDCALL *UnregisterApplication)(IDirectPlayLobby3A *self, DWORD flags, REFGUID application);
    /* 18 */ HRESULT (SFCOP_STDCALL *WaitForConnectionSettings)(IDirectPlayLobby3A *self, DWORD flags);
    // clang-format on
} IDirectPlayLobby3AVtbl;
struct IDirectPlayLobby3A {
    const IDirectPlayLobby3AVtbl *lpVtbl;
};
SFCOP_COM_ASSERT_SLOTS(IDirectPlayLobby3AVtbl, 19);

SFCOP_COM_END

#endif
