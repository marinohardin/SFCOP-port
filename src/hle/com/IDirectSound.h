/* IDirectSound: method order and IID from the public header include/dsound.h of Wine 9.0.
 * Written for this project in its own form; obtained from DirectSoundCreate. */
#ifndef SFCOP_HLE_COM_IDIRECTSOUND_H
#define SFCOP_HLE_COM_IDIRECTSOUND_H

#include "DSoundTypes.h"

SFCOP_COM_BEGIN

/* {279AFA83-4981-11CE-A521-0020AF0BE560} */
static const GUID kIID_IDirectSound = {0x279AFA83, 0x4981, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};

typedef struct IDirectSoundVtbl {
    // clang-format off
    /*  0 */ HRESULT (SFCOP_STDCALL *QueryInterface)(IDirectSound *self, REFIID riid, void **object);
    /*  1 */ ULONG (SFCOP_STDCALL *AddRef)(IDirectSound *self);
    /*  2 */ ULONG (SFCOP_STDCALL *Release)(IDirectSound *self);
    /*  3 */ HRESULT (SFCOP_STDCALL *CreateSoundBuffer)(IDirectSound *self, const DSBUFFERDESC *desc, IDirectSoundBuffer **buffer, IUnknown *outer);
    /*  4 */ HRESULT (SFCOP_STDCALL *GetCaps)(IDirectSound *self, DSCAPS *caps);
    /*  5 */ HRESULT (SFCOP_STDCALL *DuplicateSoundBuffer)(IDirectSound *self, IDirectSoundBuffer *original, IDirectSoundBuffer **duplicate);
    /*  6 */ HRESULT (SFCOP_STDCALL *SetCooperativeLevel)(IDirectSound *self, HWND window, DWORD level);
    /*  7 */ HRESULT (SFCOP_STDCALL *Compact)(IDirectSound *self);
    /*  8 */ HRESULT (SFCOP_STDCALL *GetSpeakerConfig)(IDirectSound *self, DWORD *config);
    /*  9 */ HRESULT (SFCOP_STDCALL *SetSpeakerConfig)(IDirectSound *self, DWORD config);
    /* 10 */ HRESULT (SFCOP_STDCALL *Initialize)(IDirectSound *self, const GUID *device_guid);
    // clang-format on
} IDirectSoundVtbl;
struct IDirectSound {
    const IDirectSoundVtbl *lpVtbl;
};
SFCOP_COM_ASSERT_SLOTS(IDirectSoundVtbl, 11);

SFCOP_COM_END

#endif
