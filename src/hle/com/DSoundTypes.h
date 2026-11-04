/* Forward declarations shared by the DirectSound interface layouts: the interfaces themselves
 * and the structures their methods take by pointer. Names from the public header
 * include/dsound.h of Wine 9.0. */
#ifndef SFCOP_HLE_COM_DSOUNDTYPES_H
#define SFCOP_HLE_COM_DSOUNDTYPES_H

#include "ComBase.h"

SFCOP_COM_BEGIN

typedef struct IDirectSound IDirectSound;
typedef struct IDirectSoundBuffer IDirectSoundBuffer;
typedef struct IDirectSound3DBuffer IDirectSound3DBuffer;
typedef struct IDirectSound3DListener IDirectSound3DListener;

typedef struct DSCAPS DSCAPS;
typedef struct DSBCAPS DSBCAPS;
typedef struct DSBUFFERDESC DSBUFFERDESC;
typedef struct WAVEFORMATEX WAVEFORMATEX;
typedef struct DS3DBUFFER DS3DBUFFER;
typedef struct DS3DLISTENER DS3DLISTENER;
typedef struct D3DVECTOR D3DVECTOR;

SFCOP_COM_END

#endif
