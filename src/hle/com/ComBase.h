/* The scalar types, calling convention and IUnknown that every COM layout in this directory
 * uses. The game is a 32-bit program: every argument below is 4 bytes on its stack, and a
 * vtable slot is 4 bytes in the guest. On a 64-bit host these structs describe slot order
 * and signatures only; how the guest's 4-byte tables reach host code is DESIGN §12.4.
 *
 * Valid C11 and C++17, with no Windows header. src/hle/README.md says how surface.tsv is
 * generated from these files and what the format rules are. */
#ifndef SFCOP_HLE_COM_COMBASE_H
#define SFCOP_HLE_COM_COMBASE_H

#include <stdint.h>

#if defined(_M_IX86) || defined(__i386__)
#define SFCOP_STDCALL __stdcall
#define SFCOP_COM_GUEST_ABI 1
#else
#define SFCOP_STDCALL
#define SFCOP_COM_GUEST_ABI 0
#endif

#ifdef __cplusplus
#define SFCOP_COM_BEGIN \
    namespace sfcop {   \
    namespace hle {     \
    namespace com {
#define SFCOP_COM_END \
    }                 \
    }                 \
    }
#define SFCOP_COM_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define SFCOP_COM_BEGIN
#define SFCOP_COM_END
#define SFCOP_COM_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

/* On the guest ABI a vtable is exactly 4 bytes per slot. */
#if SFCOP_COM_GUEST_ABI
#define SFCOP_COM_ASSERT_SLOTS(vtbl, n) SFCOP_COM_STATIC_ASSERT(sizeof(vtbl) == 4 * (n), #vtbl " slot count")
#else
#define SFCOP_COM_ASSERT_SLOTS(vtbl, n) SFCOP_COM_STATIC_ASSERT(sizeof(vtbl) == sizeof(void *) * (n), #vtbl " slot count")
#endif

SFCOP_COM_BEGIN

typedef int32_t HRESULT;
typedef uint32_t ULONG;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef int32_t BOOL;
typedef uint16_t WORD;
typedef uint8_t BYTE;
typedef float D3DVALUE;
typedef void *HANDLE;
typedef void *HWND;
typedef void *HDC;
typedef void *HINSTANCE;

typedef struct GUID {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
} GUID;
typedef const GUID *REFGUID;
typedef const GUID *REFIID;
typedef const GUID *REFCLSID;

/* Windows structures the methods take by pointer; the layouts are not part of this contract. */
typedef struct RECT RECT;
typedef struct RGNDATA RGNDATA;
typedef struct PALETTEENTRY PALETTEENTRY;

typedef struct IUnknown IUnknown;
typedef struct IUnknownVtbl {
    // clang-format off
    /*  0 */ HRESULT (SFCOP_STDCALL *QueryInterface)(IUnknown *self, REFIID riid, void **object);
    /*  1 */ ULONG (SFCOP_STDCALL *AddRef)(IUnknown *self);
    /*  2 */ ULONG (SFCOP_STDCALL *Release)(IUnknown *self);
    // clang-format on
} IUnknownVtbl;
struct IUnknown {
    const IUnknownVtbl *lpVtbl;
};
SFCOP_COM_ASSERT_SLOTS(IUnknownVtbl, 3);

SFCOP_COM_END

#endif
