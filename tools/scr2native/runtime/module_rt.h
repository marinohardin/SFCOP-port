/* Forced first into every C file of a translated module (clang -include). The lifted code and
 * pcrecomp's recomp_types.h name the guest machine's registers and the lifter's runtime as globals
 * of the program they are linked into. A module is a shared library of its own, so each such name
 * is made a pointer the module holds and the client fills (bind() in module_glue.c):
 *
 *     #define g_eax (*sfcop_rt_g_eax)
 *
 * turns `extern uint32_t g_eax;` into the declaration of a pointer and every use into a load
 * through it. The list is SFCOP_RT_SYMBOLS below; the module asks the client for each by its own
 * name. Data the module may keep for itself (the CPUID mask, the alias entry) is left alone.
 *
 * Built with -fcommon: a function declaration becomes a tentative definition of a pointer in
 * every file, merged into one. */
#ifndef SFCOP_SCR2NATIVE_MODULE_RT_H
#define SFCOP_SCR2NATIVE_MODULE_RT_H

/* X(name): every runtime symbol a module binds, data and functions alike. */
#define SFCOP_RT_SYMBOLS(X)                                                                       \
    X(g_eax) X(g_ecx) X(g_edx) X(g_ebx) X(g_esp) X(g_ebp) X(g_esi) X(g_edi)                       \
    X(g_st) X(g_st_i64) X(g_fp_top) X(g_fpu_cw) X(g_mm) X(g_xmm) X(g_mxcsr)                       \
    X(g_flag_k) X(g_flag_a) X(g_flag_b) X(g_flag_cf)                                              \
    X(g_seg_cs) X(g_seg_ds) X(g_seg_es) X(g_seg_fs) X(g_seg_gs) X(g_seg_ss)                       \
    X(g_fs_base) X(g_gs_base) X(g_cur_func)                                                       \
    X(g_icall_trace) X(g_icall_from) X(g_icall_trace_idx) X(g_icall_count)                        \
    X(g_backedges) X(recomp_yield_hook) X(recomp_div_fault_hook)                                  \
    X(recomp_lookup) X(recomp_lookup_manual) X(recomp_lookup_import)                              \
    X(recomp_eh_push) X(recomp_eh_pop)                                                            \
    X(sfcop_eh_jump_to_continuation) X(sfcop_eh_enter_except)

#define g_eax (*sfcop_rt_g_eax)
#define g_ecx (*sfcop_rt_g_ecx)
#define g_edx (*sfcop_rt_g_edx)
#define g_ebx (*sfcop_rt_g_ebx)
#define g_esp (*sfcop_rt_g_esp)
#define g_ebp (*sfcop_rt_g_ebp)
#define g_esi (*sfcop_rt_g_esi)
#define g_edi (*sfcop_rt_g_edi)
#define g_st (*sfcop_rt_g_st)
#define g_st_i64 (*sfcop_rt_g_st_i64)
#define g_fp_top (*sfcop_rt_g_fp_top)
#define g_fpu_cw (*sfcop_rt_g_fpu_cw)
#define g_mm (*sfcop_rt_g_mm)
#define g_xmm (*sfcop_rt_g_xmm)
#define g_mxcsr (*sfcop_rt_g_mxcsr)
#define g_flag_k (*sfcop_rt_g_flag_k)
#define g_flag_a (*sfcop_rt_g_flag_a)
#define g_flag_b (*sfcop_rt_g_flag_b)
#define g_flag_cf (*sfcop_rt_g_flag_cf)
#define g_seg_cs (*sfcop_rt_g_seg_cs)
#define g_seg_ds (*sfcop_rt_g_seg_ds)
#define g_seg_es (*sfcop_rt_g_seg_es)
#define g_seg_fs (*sfcop_rt_g_seg_fs)
#define g_seg_gs (*sfcop_rt_g_seg_gs)
#define g_seg_ss (*sfcop_rt_g_seg_ss)
#define g_gs_base (*sfcop_rt_g_gs_base)
#define g_fs_base (*sfcop_rt_g_fs_base)
#define g_cur_func (*sfcop_rt_g_cur_func)
#define g_icall_trace (*sfcop_rt_g_icall_trace)
#define g_icall_from (*sfcop_rt_g_icall_from)
#define g_icall_trace_idx (*sfcop_rt_g_icall_trace_idx)
#define g_icall_count (*sfcop_rt_g_icall_count)
#define g_backedges (*sfcop_rt_g_backedges)
#define recomp_yield_hook (*sfcop_rt_recomp_yield_hook)
#define recomp_div_fault_hook (*sfcop_rt_recomp_div_fault_hook)
#define recomp_lookup (*sfcop_rt_recomp_lookup)
#define recomp_lookup_manual (*sfcop_rt_recomp_lookup_manual)
#define recomp_lookup_import (*sfcop_rt_recomp_lookup_import)
#define recomp_eh_push (*sfcop_rt_recomp_eh_push)
#define recomp_eh_pop (*sfcop_rt_recomp_eh_pop)
#define sfcop_eh_jump_to_continuation (*sfcop_rt_sfcop_eh_jump_to_continuation)
#define sfcop_eh_enter_except (*sfcop_rt_sfcop_eh_enter_except)

#endif
