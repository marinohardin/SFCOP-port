/* Guest exceptions on win32hle (sfcop_eh.c). */
#pragma once
#include <stdint.h>

/* Register RaiseException, RtlUnwind and IsBad*Ptr. Call before
 * recomp_host_init(): the first registration of a name wins. */
void sfcop_eh_install(void);

/* The lift's hooks (run_lift_eh.py): never return. */
void sfcop_eh_jump_to_continuation(void);
void sfcop_eh_enter_except(uint32_t target);

/* 1 if guest [va, va+n) is mapped with read (and write) access. */
int sfcop_eh_guest_range_ok(uint32_t va, uint32_t n, int write);

/* A host fault on guest memory, raised as the guest's exception `code`
 * (0xC0000005) from the host's signal handler. Returns 0 if no guest frame
 * takes it; otherwise it does not return. */
int sfcop_eh_guest_fault(uint32_t code, uint32_t addr, int write);

typedef struct { uint32_t raises, faults, transfers, unwinds, unhandled; } sfcop_eh_stats_t;
extern sfcop_eh_stats_t sfcop_eh_stats;

/* recomp_eh_table.c, written by the lift: {continuation, owner function}. */
extern const uint32_t sfcop_eh_cont_table[][2];
extern const uint32_t sfcop_eh_cont_count;
