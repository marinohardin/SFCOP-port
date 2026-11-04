/* Forward declarations shared by the DirectDraw interface layouts: the interfaces themselves,
 * the structures their methods take by pointer, and the enumeration callbacks. Names and
 * callback signatures from the public header include/ddraw.h of Wine 9.0. */
#ifndef SFCOP_HLE_COM_DDRAWTYPES_H
#define SFCOP_HLE_COM_DDRAWTYPES_H

#include "ComBase.h"

SFCOP_COM_BEGIN

typedef struct IDirectDraw IDirectDraw;
typedef struct IDirectDraw2 IDirectDraw2;
typedef struct IDirectDraw4 IDirectDraw4;
typedef struct IDirectDrawSurface IDirectDrawSurface;
typedef struct IDirectDrawSurface2 IDirectDrawSurface2;
typedef struct IDirectDrawSurface3 IDirectDrawSurface3;
typedef struct IDirectDrawSurface4 IDirectDrawSurface4;
typedef struct IDirectDrawPalette IDirectDrawPalette;
typedef struct IDirectDrawClipper IDirectDrawClipper;

/* Structures passed by pointer; their layouts are not part of this contract. */
typedef struct DDSURFACEDESC DDSURFACEDESC;
typedef struct DDSURFACEDESC2 DDSURFACEDESC2;
typedef struct DDCAPS DDCAPS;
typedef struct DDPIXELFORMAT DDPIXELFORMAT;
typedef struct DDCOLORKEY DDCOLORKEY;
typedef struct DDBLTFX DDBLTFX;
typedef struct DDOVERLAYFX DDOVERLAYFX;
typedef struct DDBLTBATCH DDBLTBATCH;
typedef struct DDSCAPS DDSCAPS;
typedef struct DDSCAPS2 DDSCAPS2;
typedef struct DDDEVICEIDENTIFIER DDDEVICEIDENTIFIER;

/* Enumeration callbacks. */
typedef HRESULT (SFCOP_STDCALL *LPDDENUMMODESCALLBACK)(DDSURFACEDESC *surface_desc, void *context);
typedef HRESULT (SFCOP_STDCALL *LPDDENUMMODESCALLBACK2)(DDSURFACEDESC2 *surface_desc, void *context);
typedef HRESULT (SFCOP_STDCALL *LPDDENUMSURFACESCALLBACK)(IDirectDrawSurface *surface, DDSURFACEDESC *surface_desc, void *context);
typedef HRESULT (SFCOP_STDCALL *LPDDENUMSURFACESCALLBACK2)(IDirectDrawSurface4 *surface, DDSURFACEDESC2 *surface_desc, void *context);

SFCOP_COM_END

#endif
