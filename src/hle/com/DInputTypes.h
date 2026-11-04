/* Forward declarations shared by the DirectInput interface layouts: the interfaces themselves,
 * the structures their methods take by pointer, and the enumeration callbacks. Names and
 * callback signatures from the public header include/dinput.h of Wine 9.0. */
#ifndef SFCOP_HLE_COM_DINPUTTYPES_H
#define SFCOP_HLE_COM_DINPUTTYPES_H

#include "ComBase.h"

SFCOP_COM_BEGIN

typedef struct IDirectInputA IDirectInputA;
typedef struct IDirectInputDeviceA IDirectInputDeviceA;

typedef struct DIDEVICEINSTANCEA DIDEVICEINSTANCEA;
typedef struct DIDEVICEOBJECTINSTANCEA DIDEVICEOBJECTINSTANCEA;
typedef struct DIDEVCAPS DIDEVCAPS;
typedef struct DIPROPHEADER DIPROPHEADER;
typedef struct DIDEVICEOBJECTDATA DIDEVICEOBJECTDATA;
typedef struct DIDATAFORMAT DIDATAFORMAT;

typedef BOOL (SFCOP_STDCALL *LPDIENUMDEVICESCALLBACKA)(const DIDEVICEINSTANCEA *instance, void *context);
typedef BOOL (SFCOP_STDCALL *LPDIENUMDEVICEOBJECTSCALLBACKA)(const DIDEVICEOBJECTINSTANCEA *object, void *context);

SFCOP_COM_END

#endif
