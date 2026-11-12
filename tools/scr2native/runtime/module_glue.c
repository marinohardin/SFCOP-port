/* The part of every translated module that is not lifted (tools/scr2native/README.md): the
 * runtime binding, the version tag, the guest image and the contract's two exports.
 *
 * scr2native compiles it with the module's generated image (module_image.c) and these
 * definitions:
 *   SFCOP_ABI_TAG          the host ABI tag, "arm64-macos-itanium"
 *   SFCOP_SCRIPT_VERSION   the scripting library version, 22.0f
 *   SFCOP_CONTRACT_VERSION the module contract's version
 * module_rt.h comes first (-include), so the registers below are the client's. */
#include "recomp_types.h"
#include "GuestModuleAbi.h"

#include <string.h>

#define SFCOP_EXPORT __attribute__((visibility("default")))

/* ---- the runtime ---------------------------------------------------------------------------- */

/* recomp_funcs.h's, which the glue does not include. */
void sfcop_eh_jump_to_continuation(void);
void sfcop_eh_enter_except(uint32_t target);

#define SFCOP_RT_DEFINE(n) __typeof__(sfcop_rt_##n) sfcop_rt_##n;
SFCOP_RT_SYMBOLS(SFCOP_RT_DEFINE)

static int g_bound;

/* Binds once. Lifted code reads the pointers through a const view (module_rt_direct.h) and may
 * keep them in registers across calls, so the pointers must not change after the first bind. A
 * later bind that resolves every name to the same address is accepted and writes nothing: that is
 * a reload of a library the system kept resident (dlclose need not unload it). One that resolves
 * any name elsewhere, another runtime in the same process, is refused, and the module keeps its
 * first binding. */
const char *sfcop_bind(sfcop_guest_resolver resolve) {
#define SFCOP_RT_BIND(n)                                                                          \
    {                                                                                             \
        void *const at = resolve(#n);                                                             \
        if (!at) return #n;                                                                       \
        if (!g_bound) sfcop_rt_##n = (__typeof__(sfcop_rt_##n))at;                                \
        else if ((void *)sfcop_rt_##n != at) return "sfcop_bind (bound to another runtime)";      \
    }
    SFCOP_RT_SYMBOLS(SFCOP_RT_BIND)
    g_bound = 1;
    return 0;
}

/* ---- the image (module_image.c) ------------------------------------------------------------- */

extern const recomp_dispatch_entry_t recomp_dispatch_table[];
extern const uint32_t recomp_dispatch_count;

static recomp_func_t own(uint32_t address) {
    uint32_t lo = 0, hi = recomp_dispatch_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2, at = recomp_dispatch_table[mid].address;
        if (at == address) return recomp_dispatch_table[mid].func;
        if (at < address) lo = mid + 1; else hi = mid;
    }
    return 0;
}

uint32_t sfcop_call(uint32_t address, uint32_t argc, const uint32_t *args) {
    recomp_func_t fn = own(address);
    if (!g_bound || !fn) return 0;
    const uint32_t saved[8] = {g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi};
    const uint32_t caller = g_cur_func;
    for (uint32_t i = argc; i > 0; i--) {
        g_esp -= 4;
        MEM32(g_esp) = args[i - 1];
    }
    g_esp -= 4;
    MEM32(g_esp) = RECOMP_RETADDR;
    fn();
    const uint32_t result = g_eax;
    g_eax = saved[0], g_ecx = saved[1], g_edx = saved[2], g_ebx = saved[3];
    g_esp = saved[4], g_ebp = saved[5], g_esi = saved[6], g_edi = saved[7];
    g_cur_func = caller;
    return result;
}

/* The guest image itself, with sfcop_bind and sfcop_call in it, is module_image.c's: it holds
 * the image's numbers as constants. */
extern const sfcop_guest_image sfcop_module_guest;

/* ---- the version tag (ModuleContract.md §2) ------------------------------------------------- */

typedef struct {
    char magic[8];
    uint32_t contractVersion;
    float scriptVersion;
    char compilerAbi[32];
} sfcop_abi_tag;

SFCOP_EXPORT const sfcop_abi_tag sfcop_module_abi = {
    {'S', 'F', 'C', 'O', 'P', 'M', 'O', 'D'}, SFCOP_CONTRACT_VERSION, SFCOP_SCRIPT_VERSION,
    SFCOP_ABI_TAG};

/* ---- the contract's exports (ModuleContract.md §1) ------------------------------------------ */

/* Each runs the lifted export on the calling thread's guest machine and returns its result as a
 * host pointer into guest memory. The game itself calls the guest addresses, not these. */
SFCOP_EXPORT const void *MissionSpecEntry(float version) {
    uint32_t bits;
    memcpy(&bits, &version, sizeof bits);
    const uint32_t spec = sfcop_call(sfcop_module_guest.mission_spec_entry, 1, &bits);
    return spec ? (const void *)ADDR(spec) : 0;
}

SFCOP_EXPORT void *ScriptEntry(void) {
    const uint32_t entry = sfcop_call(sfcop_module_guest.script_entry, 0, 0);
    return entry ? (void *)ADDR(entry) : 0;
}
