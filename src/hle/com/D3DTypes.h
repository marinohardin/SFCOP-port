/* Forward declarations shared by the Direct3D interface layouts: the interfaces themselves,
 * the structures their methods take by pointer, the enumeration types and the callbacks.
 * Names and callback signatures from the public headers include/d3d.h, include/d3dtypes.h
 * and include/d3dcaps.h of Wine 9.0. */
#ifndef SFCOP_HLE_COM_D3DTYPES_H
#define SFCOP_HLE_COM_D3DTYPES_H

#include "ComBase.h"

SFCOP_COM_BEGIN

typedef struct IDirect3D IDirect3D;
typedef struct IDirect3D3 IDirect3D3;
typedef struct IDirect3DDevice2 IDirect3DDevice2;
typedef struct IDirect3DDevice3 IDirect3DDevice3;
typedef struct IDirect3DTexture2 IDirect3DTexture2;
typedef struct IDirect3DViewport3 IDirect3DViewport3;
typedef struct IDirect3DLight IDirect3DLight;
typedef struct IDirect3DMaterial3 IDirect3DMaterial3;
typedef struct IDirect3DVertexBuffer IDirect3DVertexBuffer;
typedef struct IDirectDraw4 IDirectDraw4;
typedef struct IDirectDrawSurface IDirectDrawSurface;
typedef struct IDirectDrawSurface4 IDirectDrawSurface4;

/* Structures the methods take by pointer; the layouts are not part of this contract. */
typedef struct D3DDEVICEDESC D3DDEVICEDESC;
typedef struct D3DSTATS D3DSTATS;
typedef struct D3DMATRIX D3DMATRIX;
typedef struct D3DVECTOR D3DVECTOR;
typedef struct D3DRECT D3DRECT;
typedef struct D3DCLIPSTATUS D3DCLIPSTATUS;
typedef struct D3DVIEWPORT D3DVIEWPORT;
typedef struct D3DVIEWPORT2 D3DVIEWPORT2;
typedef struct D3DLIGHTDATA D3DLIGHTDATA;
typedef struct D3DTRANSFORMDATA D3DTRANSFORMDATA;
typedef struct D3DDRAWPRIMITIVESTRIDEDDATA D3DDRAWPRIMITIVESTRIDEDDATA;
typedef struct D3DFINDDEVICESEARCH D3DFINDDEVICESEARCH;
typedef struct D3DFINDDEVICERESULT D3DFINDDEVICERESULT;
typedef struct D3DVERTEXBUFFERDESC D3DVERTEXBUFFERDESC;
typedef struct DDPIXELFORMAT DDPIXELFORMAT;
typedef struct DDSURFACEDESC2 DDSURFACEDESC2;

/* These enumerations are 4 bytes on the guest stack, so they are DWORDs here. */
typedef DWORD D3DPRIMITIVETYPE;
typedef DWORD D3DVERTEXTYPE;
typedef DWORD D3DRENDERSTATETYPE;
typedef DWORD D3DLIGHTSTATETYPE;
typedef DWORD D3DTRANSFORMSTATETYPE;
typedef DWORD D3DTEXTURESTAGESTATETYPE;

/* Colours and handles are 32-bit values. */
typedef DWORD D3DCOLOR;
typedef DWORD D3DTEXTUREHANDLE;
typedef DWORD D3DMATERIALHANDLE;

typedef HRESULT (SFCOP_STDCALL *LPD3DENUMDEVICESCALLBACK)(GUID *guid, char *description, char *name, D3DDEVICEDESC *hal_desc, D3DDEVICEDESC *hel_desc, void *context);
typedef HRESULT (SFCOP_STDCALL *LPD3DENUMPIXELFORMATSCALLBACK)(DDPIXELFORMAT *format, void *context);

SFCOP_COM_END

#endif
