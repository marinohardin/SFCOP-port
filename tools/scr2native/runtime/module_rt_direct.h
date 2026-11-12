/* Forced second, after module_rt.h, into the lifted C files of a translated module only (never
 * into module_glue.c or module_image.c). It lets the compiler keep the runtime's addresses in
 * registers instead of reloading them after every store.
 *
 * module_rt.h makes each runtime name a pointer the module holds, `uint32_t *sfcop_rt_g_eax`,
 * filled once by bind() before any lifted code runs. Lifted code stores to guest registers and
 * memory all the time; built with -fno-strict-aliasing, every such store may have changed the
 * pointer itself, so the compiler reloads it before the next use. Here the lifted files see the
 * same pointer objects as const:
 *
 *     extern uint32_t *const sfcop_rtc_g_eax __asm__("_sfcop_rt_g_eax");
 *     #define g_eax (*sfcop_rtc_g_eax)
 *
 * Same symbol, same storage, same imports and exports: the module contract does not change. Only
 * the lifted files' view of the pointers is const, so the compiler loads each once per function.
 *
 * Why this is sound: the glue defines the pointers (non-const) and writes them only in bind(),
 * which the client calls before it calls into the module, and which never changes them once
 * bound (a later bind to other addresses is refused). Lifted code never
 * writes them. The const view differs from the definition in qualifier only, across translation
 * units, which is fine for separately compiled objects; a module must not be built with
 * link-time optimisation, which could see both and treat the mismatch as undefined, so
 * compile_flags() passes -fno-lto.
 *
 * __typeof__, __asm__ labels and __USER_LABEL_PREFIX__ are GCC and clang extensions, which every
 * host compiler of a module is today; the probe test in tests/test_scr2native.py checks the label
 * on the host it runs on, so a new host compiler is checked when the tests run there.
 *
 * pcrecomp's own headers are included first so their declarations are read with module_rt.h's
 * macros, as before; their inline helpers keep using the non-const pointers. */
#ifndef SFCOP_SCR2NATIVE_MODULE_RT_DIRECT_H
#define SFCOP_SCR2NATIVE_MODULE_RT_DIRECT_H

#ifndef RECOMP_GENERATED_CODE
#define RECOMP_GENERATED_CODE
#endif
#include "recomp_types.h"
#include "recomp_funcs.h"

#define SFCOP_RT_STR2(x) #x
#define SFCOP_RT_STR(x) SFCOP_RT_STR2(x)
#define SFCOP_RT_CONST(n)                                                                         \
    extern __typeof__(sfcop_rt_##n) const sfcop_rtc_##n                                           \
        __asm__(SFCOP_RT_STR(__USER_LABEL_PREFIX__) "sfcop_rt_" #n);
SFCOP_RT_SYMBOLS(SFCOP_RT_CONST)

/* One pair per name in SFCOP_RT_SYMBOLS, same order (tests/test_scr2native.py checks). */
#undef g_eax
#define g_eax (*sfcop_rtc_g_eax)
#undef g_ecx
#define g_ecx (*sfcop_rtc_g_ecx)
#undef g_edx
#define g_edx (*sfcop_rtc_g_edx)
#undef g_ebx
#define g_ebx (*sfcop_rtc_g_ebx)
#undef g_esp
#define g_esp (*sfcop_rtc_g_esp)
#undef g_ebp
#define g_ebp (*sfcop_rtc_g_ebp)
#undef g_esi
#define g_esi (*sfcop_rtc_g_esi)
#undef g_edi
#define g_edi (*sfcop_rtc_g_edi)
#undef g_st
#define g_st (*sfcop_rtc_g_st)
#undef g_st_i64
#define g_st_i64 (*sfcop_rtc_g_st_i64)
#undef g_fp_top
#define g_fp_top (*sfcop_rtc_g_fp_top)
#undef g_fpu_cw
#define g_fpu_cw (*sfcop_rtc_g_fpu_cw)
#undef g_mm
#define g_mm (*sfcop_rtc_g_mm)
#undef g_xmm
#define g_xmm (*sfcop_rtc_g_xmm)
#undef g_mxcsr
#define g_mxcsr (*sfcop_rtc_g_mxcsr)
#undef g_flag_k
#define g_flag_k (*sfcop_rtc_g_flag_k)
#undef g_flag_a
#define g_flag_a (*sfcop_rtc_g_flag_a)
#undef g_flag_b
#define g_flag_b (*sfcop_rtc_g_flag_b)
#undef g_flag_cf
#define g_flag_cf (*sfcop_rtc_g_flag_cf)
#undef g_seg_cs
#define g_seg_cs (*sfcop_rtc_g_seg_cs)
#undef g_seg_ds
#define g_seg_ds (*sfcop_rtc_g_seg_ds)
#undef g_seg_es
#define g_seg_es (*sfcop_rtc_g_seg_es)
#undef g_seg_fs
#define g_seg_fs (*sfcop_rtc_g_seg_fs)
#undef g_seg_gs
#define g_seg_gs (*sfcop_rtc_g_seg_gs)
#undef g_seg_ss
#define g_seg_ss (*sfcop_rtc_g_seg_ss)
#undef g_fs_base
#define g_fs_base (*sfcop_rtc_g_fs_base)
#undef g_gs_base
#define g_gs_base (*sfcop_rtc_g_gs_base)
#undef g_cur_func
#define g_cur_func (*sfcop_rtc_g_cur_func)
#undef g_icall_trace
#define g_icall_trace (*sfcop_rtc_g_icall_trace)
#undef g_icall_from
#define g_icall_from (*sfcop_rtc_g_icall_from)
#undef g_icall_trace_idx
#define g_icall_trace_idx (*sfcop_rtc_g_icall_trace_idx)
#undef g_icall_count
#define g_icall_count (*sfcop_rtc_g_icall_count)
#undef g_backedges
#define g_backedges (*sfcop_rtc_g_backedges)
#undef recomp_yield_hook
#define recomp_yield_hook (*sfcop_rtc_recomp_yield_hook)
#undef recomp_div_fault_hook
#define recomp_div_fault_hook (*sfcop_rtc_recomp_div_fault_hook)
#undef recomp_lookup
#define recomp_lookup (*sfcop_rtc_recomp_lookup)
#undef recomp_lookup_manual
#define recomp_lookup_manual (*sfcop_rtc_recomp_lookup_manual)
#undef recomp_lookup_import
#define recomp_lookup_import (*sfcop_rtc_recomp_lookup_import)
#undef recomp_eh_push
#define recomp_eh_push (*sfcop_rtc_recomp_eh_push)
#undef recomp_eh_pop
#define recomp_eh_pop (*sfcop_rtc_recomp_eh_pop)
#undef sfcop_eh_jump_to_continuation
#define sfcop_eh_jump_to_continuation (*sfcop_rtc_sfcop_eh_jump_to_continuation)
#undef sfcop_eh_enter_except
#define sfcop_eh_enter_except (*sfcop_rtc_sfcop_eh_enter_except)

#endif
