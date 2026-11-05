/* The guest image a translated mission module carries (ModuleContract.md §1): what scr2native
 * writes into the module and what the client reads from it to put the mission in the game's
 * 32-bit guest address space. C, so the module's generated C and the client's C++ share it.
 *
 * A translated module is the original .scr lifted to C at a guest base of its own: its sections,
 * relocated to that base, are copied into guest memory there; its lifted functions answer the
 * guest addresses they were lifted from; its Windows imports are bound, slot by slot, to the
 * client's rows. The module reaches the game's machine (the registers, the x87 stack, the lifter's
 * dispatch) only through the runtime it is bound to, by name, before any of its code runs.
 *
 * Threading: everything here is used with the guest machine held. */
#ifndef SFCOP_APP_SCRIPTS_GUEST_MODULE_ABI_H
#define SFCOP_APP_SCRIPTS_GUEST_MODULE_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The layout version of sfcop_guest_image; a module of another version is refused. */
#define SFCOP_GUEST_ABI_VERSION 1u

/* The exported data symbol that holds the image, beside the contract's exports. */
#define SFCOP_GUEST_IMAGE_SYMBOL "sfcop_module_guest"

/* Where translated modules live in the guest: 1 MB slots from 0x60000000, the n-th .scr of its
 * folder (in byte order of the file names) in slot n. Nothing else of the game's maps there: the
 * exe ends below 0x00C86000, lifted objects take 0x0F000000 to 0x10000000, the guest allocator
 * hands out reservations upward from 0x10000000, and the host's rows sit at 0x7F000000. */
#define SFCOP_GUEST_MODULE_FIRST 0x60000000u
#define SFCOP_GUEST_MODULE_SLOT 0x00100000u
#define SFCOP_GUEST_MODULE_SLOTS 256u

typedef void (*sfcop_guest_fn)(void);

/* A lifted function at the guest address it was lifted from. Sorted by address. */
typedef struct sfcop_guest_dispatch {
    uint32_t address;
    sfcop_guest_fn func;
} sfcop_guest_dispatch;

/* Part of the image: `size` bytes at base + rva, the first `data_size` of them from `data`, the
 * rest zero. */
typedef struct sfcop_guest_segment {
    uint32_t rva;
    uint32_t size;
    uint32_t data_size;
    const uint8_t *data;
} sfcop_guest_segment;

/* A Windows import: the slot at base + iat_rva gets the guest address of the client's row for
 * `name`, written DLL!Name as on the allow-list. */
typedef struct sfcop_guest_import {
    uint32_t iat_rva;
    const char *name;
} sfcop_guest_import;

/* The client's runtime by name: the address of a symbol of the running client, or null. */
typedef void *(*sfcop_guest_resolver)(const char *name);

typedef struct sfcop_module_guest {
    uint32_t abi_version;        /* SFCOP_GUEST_ABI_VERSION */
    uint32_t base;               /* the guest base the module was lifted at */
    uint32_t size;               /* the image's size from base, a multiple of 64 KB */
    uint32_t entry;              /* the guest address of the DLL entry point, 0 when none */
    uint32_t mission_spec_entry; /* the guest addresses of the contract's two exports */
    uint32_t script_entry;
    const sfcop_guest_segment *segments;
    uint32_t segment_count;
    const sfcop_guest_import *imports;
    uint32_t import_count;
    const sfcop_guest_dispatch *dispatch;
    uint32_t dispatch_count;
    /* Binds the module to the client's runtime; returns null when bound, else the name of the
     * first symbol `resolve` did not give. No lifted code runs before it succeeds. */
    const char *(*bind)(sfcop_guest_resolver resolve);
    /* Runs the module's function at `address` on the calling thread's guest machine with `argc`
     * 32-bit arguments pushed right to left, and returns eax. The registers are as they were
     * after it returns, whatever the function's calling convention. 0 when no function is there. */
    uint32_t (*call)(uint32_t address, uint32_t argc, const uint32_t *args);
} sfcop_guest_image;

#ifdef __cplusplus
}
#endif

#endif
