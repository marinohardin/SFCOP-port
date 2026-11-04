/* Forward declarations shared by the DirectPlay and DirectPlayLobby interface layouts: the
 * interfaces themselves, the structures their methods take by pointer, and the enumeration
 * callbacks. Names and callback signatures from the public headers include/dplay.h and
 * include/dplobby.h of Wine 9.0. */
#ifndef SFCOP_HLE_COM_DPLAYTYPES_H
#define SFCOP_HLE_COM_DPLAYTYPES_H

#include "ComBase.h"

SFCOP_COM_BEGIN

typedef DWORD DPID;

typedef struct IDirectPlay4A IDirectPlay4A;
typedef struct IDirectPlayLobby3A IDirectPlayLobby3A;

/* Structures the methods take by pointer; the layouts are not part of this contract. */
typedef struct DPCAPS DPCAPS;
typedef struct DPNAME DPNAME;
typedef struct DPSESSIONDESC2 DPSESSIONDESC2;
typedef struct DPLCONNECTION DPLCONNECTION;
typedef struct DPCHAT DPCHAT;
typedef struct DPSECURITYDESC DPSECURITYDESC;
typedef struct DPCREDENTIALS DPCREDENTIALS;
typedef struct DPCOMPOUNDADDRESSELEMENT DPCOMPOUNDADDRESSELEMENT;
typedef struct DPAPPLICATIONDESC DPAPPLICATIONDESC;
typedef struct DPLAPPINFO DPLAPPINFO;

// clang-format off
typedef BOOL (SFCOP_STDCALL *LPDPENUMSESSIONSCALLBACK2)(const DPSESSIONDESC2 *session, DWORD *timeout, DWORD flags, void *context);
typedef BOOL (SFCOP_STDCALL *LPDPENUMPLAYERSCALLBACK2)(DPID id, DWORD player_type, const DPNAME *name, DWORD flags, void *context);
typedef BOOL (SFCOP_STDCALL *LPDPENUMDPCALLBACKA)(GUID *provider, char *provider_name, DWORD major_version, DWORD minor_version, void *context);
typedef BOOL (SFCOP_STDCALL *LPDPENUMCONNECTIONSCALLBACK)(const GUID *provider, void *connection, DWORD connection_size, const DPNAME *name, DWORD flags, void *context);
typedef BOOL (SFCOP_STDCALL *LPDPENUMADDRESSCALLBACK)(REFGUID data_type, DWORD data_size, const void *data, void *context);
typedef BOOL (SFCOP_STDCALL *LPDPLENUMADDRESSTYPESCALLBACK)(REFGUID data_type, void *context, DWORD flags);
typedef BOOL (SFCOP_STDCALL *LPDPLENUMLOCALAPPLICATIONSCALLBACK)(const DPLAPPINFO *app_info, void *context, DWORD flags);
// clang-format on

SFCOP_COM_END

#endif
