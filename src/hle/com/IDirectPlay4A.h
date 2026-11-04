/* IDirectPlay4A: method order and IID from the public header include/dplay.h of Wine 9.0.
 * Written for this project in its own form; obtained from CoCreateInstance(CLSID_DirectPlay)
 * and IDirectPlayLobby3A::ConnectEx. */
#ifndef SFCOP_HLE_COM_IDIRECTPLAY4A_H
#define SFCOP_HLE_COM_IDIRECTPLAY4A_H

#include "DPlayTypes.h"

SFCOP_COM_BEGIN

/* {D1EB6D20-8923-11D0-9D97-00A0C90A43CB} */
static const GUID kCLSID_DirectPlay = {0xD1EB6D20, 0x8923, 0x11D0, {0x9D, 0x97, 0x00, 0xA0, 0xC9, 0x0A, 0x43, 0xCB}};

/* {0AB1C531-4745-11D1-A7A1-0000F803ABFC} */
static const GUID kIID_IDirectPlay4A = {0x0AB1C531, 0x4745, 0x11D1, {0xA7, 0xA1, 0x00, 0x00, 0xF8, 0x03, 0xAB, 0xFC}};

typedef struct IDirectPlay4AVtbl {
    // clang-format off
    /*  0 */ HRESULT (SFCOP_STDCALL *QueryInterface)(IDirectPlay4A *self, REFIID riid, void **object);
    /*  1 */ ULONG (SFCOP_STDCALL *AddRef)(IDirectPlay4A *self);
    /*  2 */ ULONG (SFCOP_STDCALL *Release)(IDirectPlay4A *self);
    /*  3 */ HRESULT (SFCOP_STDCALL *AddPlayerToGroup)(IDirectPlay4A *self, DPID group, DPID player);
    /*  4 */ HRESULT (SFCOP_STDCALL *Close)(IDirectPlay4A *self);
    /*  5 */ HRESULT (SFCOP_STDCALL *CreateGroup)(IDirectPlay4A *self, DPID *group, DPNAME *name, void *data, DWORD data_size, DWORD flags);
    /*  6 */ HRESULT (SFCOP_STDCALL *CreatePlayer)(IDirectPlay4A *self, DPID *player, DPNAME *name, HANDLE event, void *data, DWORD data_size, DWORD flags);
    /*  7 */ HRESULT (SFCOP_STDCALL *DeletePlayerFromGroup)(IDirectPlay4A *self, DPID group, DPID player);
    /*  8 */ HRESULT (SFCOP_STDCALL *DestroyGroup)(IDirectPlay4A *self, DPID group);
    /*  9 */ HRESULT (SFCOP_STDCALL *DestroyPlayer)(IDirectPlay4A *self, DPID player);
    /* 10 */ HRESULT (SFCOP_STDCALL *EnumGroupPlayers)(IDirectPlay4A *self, DPID group, GUID *instance, LPDPENUMPLAYERSCALLBACK2 callback, void *context, DWORD flags);
    /* 11 */ HRESULT (SFCOP_STDCALL *EnumGroups)(IDirectPlay4A *self, GUID *instance, LPDPENUMPLAYERSCALLBACK2 callback, void *context, DWORD flags);
    /* 12 */ HRESULT (SFCOP_STDCALL *EnumPlayers)(IDirectPlay4A *self, GUID *instance, LPDPENUMPLAYERSCALLBACK2 callback, void *context, DWORD flags);
    /* 13 */ HRESULT (SFCOP_STDCALL *EnumSessions)(IDirectPlay4A *self, DPSESSIONDESC2 *filter, DWORD timeout, LPDPENUMSESSIONSCALLBACK2 callback, void *context, DWORD flags);
    /* 14 */ HRESULT (SFCOP_STDCALL *GetCaps)(IDirectPlay4A *self, DPCAPS *caps, DWORD flags);
    /* 15 */ HRESULT (SFCOP_STDCALL *GetGroupData)(IDirectPlay4A *self, DPID group, void *data, DWORD *data_size, DWORD flags);
    /* 16 */ HRESULT (SFCOP_STDCALL *GetGroupName)(IDirectPlay4A *self, DPID group, void *name, DWORD *name_size);
    /* 17 */ HRESULT (SFCOP_STDCALL *GetMessageCount)(IDirectPlay4A *self, DPID player, DWORD *count);
    /* 18 */ HRESULT (SFCOP_STDCALL *GetPlayerAddress)(IDirectPlay4A *self, DPID player, void *address, DWORD *address_size);
    /* 19 */ HRESULT (SFCOP_STDCALL *GetPlayerCaps)(IDirectPlay4A *self, DPID player, DPCAPS *caps, DWORD flags);
    /* 20 */ HRESULT (SFCOP_STDCALL *GetPlayerData)(IDirectPlay4A *self, DPID player, void *data, DWORD *data_size, DWORD flags);
    /* 21 */ HRESULT (SFCOP_STDCALL *GetPlayerName)(IDirectPlay4A *self, DPID player, void *name, DWORD *name_size);
    /* 22 */ HRESULT (SFCOP_STDCALL *GetSessionDesc)(IDirectPlay4A *self, void *desc, DWORD *desc_size);
    /* 23 */ HRESULT (SFCOP_STDCALL *Initialize)(IDirectPlay4A *self, GUID *provider);
    /* 24 */ HRESULT (SFCOP_STDCALL *Open)(IDirectPlay4A *self, DPSESSIONDESC2 *session, DWORD flags);
    /* 25 */ HRESULT (SFCOP_STDCALL *Receive)(IDirectPlay4A *self, DPID *from, DPID *to, DWORD flags, void *data, DWORD *data_size);
    /* 26 */ HRESULT (SFCOP_STDCALL *Send)(IDirectPlay4A *self, DPID from, DPID to, DWORD flags, void *data, DWORD data_size);
    /* 27 */ HRESULT (SFCOP_STDCALL *SetGroupData)(IDirectPlay4A *self, DPID group, void *data, DWORD data_size, DWORD flags);
    /* 28 */ HRESULT (SFCOP_STDCALL *SetGroupName)(IDirectPlay4A *self, DPID group, DPNAME *name, DWORD flags);
    /* 29 */ HRESULT (SFCOP_STDCALL *SetPlayerData)(IDirectPlay4A *self, DPID player, void *data, DWORD data_size, DWORD flags);
    /* 30 */ HRESULT (SFCOP_STDCALL *SetPlayerName)(IDirectPlay4A *self, DPID player, DPNAME *name, DWORD flags);
    /* 31 */ HRESULT (SFCOP_STDCALL *SetSessionDesc)(IDirectPlay4A *self, DPSESSIONDESC2 *desc, DWORD flags);
    /* 32 */ HRESULT (SFCOP_STDCALL *AddGroupToGroup)(IDirectPlay4A *self, DPID parent_group, DPID group);
    /* 33 */ HRESULT (SFCOP_STDCALL *CreateGroupInGroup)(IDirectPlay4A *self, DPID parent_group, DPID *group, DPNAME *name, void *data, DWORD data_size, DWORD flags);
    /* 34 */ HRESULT (SFCOP_STDCALL *DeleteGroupFromGroup)(IDirectPlay4A *self, DPID parent_group, DPID group);
    /* 35 */ HRESULT (SFCOP_STDCALL *EnumConnections)(IDirectPlay4A *self, const GUID *application, LPDPENUMCONNECTIONSCALLBACK callback, void *context, DWORD flags);
    /* 36 */ HRESULT (SFCOP_STDCALL *EnumGroupsInGroup)(IDirectPlay4A *self, DPID group, GUID *instance, LPDPENUMPLAYERSCALLBACK2 callback, void *context, DWORD flags);
    /* 37 */ HRESULT (SFCOP_STDCALL *GetGroupConnectionSettings)(IDirectPlay4A *self, DWORD flags, DPID group, void *settings, DWORD *settings_size);
    /* 38 */ HRESULT (SFCOP_STDCALL *InitializeConnection)(IDirectPlay4A *self, void *connection, DWORD flags);
    /* 39 */ HRESULT (SFCOP_STDCALL *SecureOpen)(IDirectPlay4A *self, const DPSESSIONDESC2 *session, DWORD flags, const DPSECURITYDESC *security, const DPCREDENTIALS *credentials);
    /* 40 */ HRESULT (SFCOP_STDCALL *SendChatMessage)(IDirectPlay4A *self, DPID from, DPID to, DWORD flags, DPCHAT *message);
    /* 41 */ HRESULT (SFCOP_STDCALL *SetGroupConnectionSettings)(IDirectPlay4A *self, DWORD flags, DPID group, DPLCONNECTION *connection);
    /* 42 */ HRESULT (SFCOP_STDCALL *StartSession)(IDirectPlay4A *self, DWORD flags, DPID group);
    /* 43 */ HRESULT (SFCOP_STDCALL *GetGroupFlags)(IDirectPlay4A *self, DPID group, DWORD *flags);
    /* 44 */ HRESULT (SFCOP_STDCALL *GetGroupParent)(IDirectPlay4A *self, DPID group, DPID *parent);
    /* 45 */ HRESULT (SFCOP_STDCALL *GetPlayerAccount)(IDirectPlay4A *self, DPID player, DWORD flags, void *account, DWORD *account_size);
    /* 46 */ HRESULT (SFCOP_STDCALL *GetPlayerFlags)(IDirectPlay4A *self, DPID player, DWORD *flags);
    /* 47 */ HRESULT (SFCOP_STDCALL *GetGroupOwner)(IDirectPlay4A *self, DPID group, DPID *owner);
    /* 48 */ HRESULT (SFCOP_STDCALL *SetGroupOwner)(IDirectPlay4A *self, DPID group, DPID owner);
    /* 49 */ HRESULT (SFCOP_STDCALL *SendEx)(IDirectPlay4A *self, DPID from, DPID to, DWORD flags, void *data, DWORD data_size, DWORD priority, DWORD timeout, void *context, DWORD *message_id);
    /* 50 */ HRESULT (SFCOP_STDCALL *GetMessageQueue)(IDirectPlay4A *self, DPID from, DPID to, DWORD flags, DWORD *messages, DWORD *bytes);
    /* 51 */ HRESULT (SFCOP_STDCALL *CancelMessage)(IDirectPlay4A *self, DWORD message_id, DWORD flags);
    /* 52 */ HRESULT (SFCOP_STDCALL *CancelPriority)(IDirectPlay4A *self, DWORD min_priority, DWORD max_priority, DWORD flags);
    // clang-format on
} IDirectPlay4AVtbl;
struct IDirectPlay4A {
    const IDirectPlay4AVtbl *lpVtbl;
};
SFCOP_COM_ASSERT_SLOTS(IDirectPlay4AVtbl, 53);

SFCOP_COM_END

#endif
