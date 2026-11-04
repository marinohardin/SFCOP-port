/* IDirectSoundBuffer: method order and IID from the public header include/dsound.h of Wine 9.0.
 * Written for this project in its own form; obtained from IDirectSound::CreateSoundBuffer. */
#ifndef SFCOP_HLE_COM_IDIRECTSOUNDBUFFER_H
#define SFCOP_HLE_COM_IDIRECTSOUNDBUFFER_H

#include "DSoundTypes.h"

SFCOP_COM_BEGIN

/* {279AFA85-4981-11CE-A521-0020AF0BE560} */
static const GUID kIID_IDirectSoundBuffer = {0x279AFA85, 0x4981, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};

typedef struct IDirectSoundBufferVtbl {
    // clang-format off
    /*  0 */ HRESULT (SFCOP_STDCALL *QueryInterface)(IDirectSoundBuffer *self, REFIID riid, void **object);
    /*  1 */ ULONG (SFCOP_STDCALL *AddRef)(IDirectSoundBuffer *self);
    /*  2 */ ULONG (SFCOP_STDCALL *Release)(IDirectSoundBuffer *self);
    /*  3 */ HRESULT (SFCOP_STDCALL *GetCaps)(IDirectSoundBuffer *self, DSBCAPS *caps);
    /*  4 */ HRESULT (SFCOP_STDCALL *GetCurrentPosition)(IDirectSoundBuffer *self, DWORD *play_cursor, DWORD *write_cursor);
    /*  5 */ HRESULT (SFCOP_STDCALL *GetFormat)(IDirectSoundBuffer *self, WAVEFORMATEX *format, DWORD buffer_size, DWORD *written_size);
    /*  6 */ HRESULT (SFCOP_STDCALL *GetVolume)(IDirectSoundBuffer *self, LONG *volume);
    /*  7 */ HRESULT (SFCOP_STDCALL *GetPan)(IDirectSoundBuffer *self, LONG *pan);
    /*  8 */ HRESULT (SFCOP_STDCALL *GetFrequency)(IDirectSoundBuffer *self, DWORD *frequency);
    /*  9 */ HRESULT (SFCOP_STDCALL *GetStatus)(IDirectSoundBuffer *self, DWORD *status);
    /* 10 */ HRESULT (SFCOP_STDCALL *Initialize)(IDirectSoundBuffer *self, IDirectSound *dsound, const DSBUFFERDESC *desc);
    /* 11 */ HRESULT (SFCOP_STDCALL *Lock)(IDirectSoundBuffer *self, DWORD offset, DWORD bytes, void **first_ptr, DWORD *first_bytes, void **second_ptr, DWORD *second_bytes, DWORD flags);
    /* 12 */ HRESULT (SFCOP_STDCALL *Play)(IDirectSoundBuffer *self, DWORD reserved1, DWORD priority, DWORD flags);
    /* 13 */ HRESULT (SFCOP_STDCALL *SetCurrentPosition)(IDirectSoundBuffer *self, DWORD position);
    /* 14 */ HRESULT (SFCOP_STDCALL *SetFormat)(IDirectSoundBuffer *self, const WAVEFORMATEX *format);
    /* 15 */ HRESULT (SFCOP_STDCALL *SetVolume)(IDirectSoundBuffer *self, LONG volume);
    /* 16 */ HRESULT (SFCOP_STDCALL *SetPan)(IDirectSoundBuffer *self, LONG pan);
    /* 17 */ HRESULT (SFCOP_STDCALL *SetFrequency)(IDirectSoundBuffer *self, DWORD frequency);
    /* 18 */ HRESULT (SFCOP_STDCALL *Stop)(IDirectSoundBuffer *self);
    /* 19 */ HRESULT (SFCOP_STDCALL *Unlock)(IDirectSoundBuffer *self, void *first_ptr, DWORD first_bytes, void *second_ptr, DWORD second_bytes);
    /* 20 */ HRESULT (SFCOP_STDCALL *Restore)(IDirectSoundBuffer *self);
    // clang-format on
} IDirectSoundBufferVtbl;
struct IDirectSoundBuffer {
    const IDirectSoundBufferVtbl *lpVtbl;
};
SFCOP_COM_ASSERT_SLOTS(IDirectSoundBufferVtbl, 21);

SFCOP_COM_END

#endif
