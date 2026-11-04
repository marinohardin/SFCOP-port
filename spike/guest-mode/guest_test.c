/*
 * guest_test.c - the guest-mode spike's guest test (C3, DESIGN §12.4).
 *
 *   sfcop_guest_test <path of StarFleetOP.exe>
 *
 * Maps the original exe into guest memory as the boot does (no entry point is run), builds a few
 * objects in guest memory from the game's own vtables and type descriptors, and calls the three
 * functions the spike claimed the way lifted code calls them:
 *
 *   0x006FC5A0 tDynamicAllocator::tAllocatorInfo::operator==(const char*) const
 *   0x006FC750 tDynamicAllocator::LockDownAllocator()
 *   0x006FC4D0 tAllocatedClass::operator==(const type_info&), through slot 2 of a guest vtable
 *
 * plus tDynamicAllocator::mGetAllocatorID(const char*) (0x006FCC10), a lifted function that calls
 * the first two directly. Every expected value below is written by hand. The same test runs in the
 * all-lifted build and in the build with SFCOP_GUEST_SOURCES; its "[test]" lines must be the same
 * in both (spike/guest-mode/run_all.sh compares them). "[path]" lines say which body answered:
 * they are the only lines that differ. Exit status: 0 when every check holds.
 */
#include <stdio.h>
#include <string.h>

#include "win32hle.h"
#include "pe_loader.h"
#include "host.h"
#include "app/guest/override.h"

/* Addresses in StarFleetOP.exe 2.5.5.2 (rtti.json of the lift; the bytes at each are quoted). */
#define VA_INFO_EQ_NAME      0x006FC5A0u  /* tAllocatorInfo::operator==(const char*) const */
#define VA_LOCKDOWN          0x006FC750u  /* tDynamicAllocator::LockDownAllocator */
#define VA_ALLOCATED_EQ_TYPE 0x006FC4D0u  /* tAllocatedClass::operator==(const type_info&) */
#define VA_GET_ID_BY_NAME    0x006FCC10u  /* tDynamicAllocator::mGetAllocatorID(const char*) */

#define VT_ALLOCATED_CLASS   0x007BB2F8u  /* tAllocatedClass: [0x006FC4A0, 0x0070E520, 0x006FC4D0] */
#define VT_SHIP              0x00798264u  /* tShip: slot 2 is 0x006FC4D0 too */
#define VT_ALLOC_SHIP        0x007B0C4Cu  /* tClassAllocator<tShip>: slot 2 mAssignAllocID */
#define VT_ALLOC_CAMPAIGN    0x007B0DD4u  /* tClassAllocator<tCampaignInfo> */
#define VT_ALLOC_BATTLES     0x007B0DC4u  /* tClassAllocator<tBattles> */
/* Each mAssignAllocID is `mov eax,[esp+4]; mov [fAllocID],eax; ret 4`: */
#define ID_SHIP              0x0085C514u  /* tClassAllocator<tShip>::fAllocID */
#define ID_CAMPAIGN          0x0085CE24u  /* tClassAllocator<tCampaignInfo>::fAllocID */
#define ID_BATTLES           0x0085C9CCu  /* tClassAllocator<tBattles>::fAllocID */

#define TD_ALLOCATED_CLASS   0x0083E060u  /* .?AVtAllocatedClass@@ */
#define TD_SHIP              0x0083E0A8u  /* .?AVtShip@@ */
#define TD_CAMPAIGN          0x008445C8u  /* .?AVtCampaignInfo@@ */
#define TD_BATTLES           0x0085C9A0u  /* .?AV?$tClassAllocator@VtBattles@@@@ (any other type) */
#define TD_ALLOCATED_PTR     0x0088C180u  /* .PAVtAllocatedClass@@ */

static int g_failed;

/* A call into the guest as a lifted caller makes it: `ecx` loaded, arguments pushed, through the
 * dispatch table (which in the guest build holds the override for a claimed address). */
static uint32_t call_guest(uint32_t va, uint32_t ecx, int nargs, const uint32_t *args) {
    mach_enter();
    uint32_t saved = g_ecx;
    g_ecx = ecx;
    uint32_t eax = hle_call_guest(va, nargs, args);
    g_ecx = saved;
    mach_leave();
    return eax;
}

static void check(const char *what, uint32_t got, uint32_t want) {
    printf("[test] %-58s %08X %s\n", what, got, got == want ? "ok" : "FAILED");
    if (got != want) {
        printf("[test]   expected %08X\n", want);
        g_failed++;
    }
}

static uint32_t guest_string(const char *s) {
    char *p = hle_alloc((uint32_t)strlen(s) + 1);
    strcpy(p, s);
    return hle_va(p);
}

static uint32_t guest_object(uint32_t vtable, uint32_t size) {
    uint32_t va = hle_va(hle_alloc(size));
    MEM32(va) = vtable;
    return va;
}

static void path(uint32_t va) {
    sfcop_guest_entry o = sfcop_guest_override_lookup(va);
    printf("[path] 0x%08X runs %s\n", va, o && recomp_lookup(va) == o
                                              ? "the override compiled from game/ (dispatch table entry replaced)"
                                              : "its lifted body");
}

/* A tDynamicAllocator in guest memory: {bool locked; std::vector<tAllocatorInfo>* list}, the
 * vector being VC6's {allocator byte; _First; _Last; _End}, each tAllocatorInfo {type, allocator}. */
static uint32_t make_registry(void) {
    static const uint32_t types[3] = {TD_SHIP, TD_CAMPAIGN, TD_BATTLES};
    static const uint32_t vtables[3] = {VT_ALLOC_SHIP, VT_ALLOC_CAMPAIGN, VT_ALLOC_BATTLES};
    uint32_t infos = hle_va(hle_alloc(3 * 8));
    for (uint32_t i = 0; i < 3; i++) {
        MEM32(infos + 8 * i) = types[i];
        MEM32(infos + 8 * i + 4) = guest_object(vtables[i], 4);
    }
    uint32_t list = hle_va(hle_alloc(16));
    MEM32(list + 4) = infos;
    MEM32(list + 8) = infos + 3 * 8;
    MEM32(list + 12) = infos + 3 * 8;
    uint32_t registry = hle_va(hle_alloc(8));
    MEM8(registry) = 0;
    MEM32(registry + 4) = list;
    MEM32(ID_SHIP) = MEM32(ID_CAMPAIGN) = MEM32(ID_BATTLES) = 0xFFFFFFFFu;
    return registry;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: sfcop_guest_test <path of StarFleetOP.exe>\n");
        return 2;
    }
    recomp_host_init();
    pe_image img;
    if (recomp_pe_map(argv[1], &img) != 0 || img.base != 0x00400000u) {
        fprintf(stderr, "cannot map %s at 0x00400000\n", argv[1]);
        return 2;
    }
    recomp_pe_bind(&img, hle_resolve_or_stub);
    printf("[path] %u functions claimed from game/ in this build\n", sfcop_guest_override_count);
    path(VA_INFO_EQ_NAME);
    path(VA_LOCKDOWN);
    path(VA_ALLOCATED_EQ_TYPE);

    /* 1. tAllocatorInfo::operator==(const char*): a pointer argument, a pointer member */
    uint32_t info = hle_va(hle_alloc(8));
    MEM32(info) = TD_SHIP;
    uint32_t a;
    a = guest_string(".?AVtShip@@");
    check("info{tShip} == \".?AVtShip@@\"", call_guest(VA_INFO_EQ_NAME, info, 1, &a), 1);
    a = guest_string(".?AVtShipX@@");
    check("info{tShip} == \".?AVtShipX@@\"", call_guest(VA_INFO_EQ_NAME, info, 1, &a), 0);
    a = guest_string(".?AVtShi");
    check("info{tShip} == \".?AVtShi\" (a prefix)", call_guest(VA_INFO_EQ_NAME, info, 1, &a), 0);
    a = guest_string("");
    check("info{tShip} == \"\"", call_guest(VA_INFO_EQ_NAME, info, 1, &a), 0);
    MEM32(info) = 0;
    a = guest_string(".?AVtShip@@");
    check("info{no type} == \".?AVtShip@@\"", call_guest(VA_INFO_EQ_NAME, info, 1, &a), 0);

    /* 2. LockDownAllocator: pointer members, a vector, a virtual call out to lifted code */
    uint32_t registry = make_registry();
    call_guest(VA_LOCKDOWN, registry, 0, NULL);
    check("LockDownAllocator: locked", MEM8(registry), 1);
    check("LockDownAllocator: tClassAllocator<tShip> got ID", MEM32(ID_SHIP), 0);
    check("LockDownAllocator: tClassAllocator<tCampaignInfo> got ID", MEM32(ID_CAMPAIGN), 1);
    check("LockDownAllocator: tClassAllocator<tBattles> got ID", MEM32(ID_BATTLES), 2);
    MEM32(ID_SHIP) = 7;
    call_guest(VA_LOCKDOWN, registry, 0, NULL);
    check("LockDownAllocator again: nothing reassigned", MEM32(ID_SHIP), 7);
    MEM32(MEM32(registry + 4) + 4) = 0;  /* _First = NULL: VC6's size() is 0 */
    MEM8(registry) = 0;
    call_guest(VA_LOCKDOWN, registry, 0, NULL);
    check("LockDownAllocator, empty vector: locked", MEM8(registry), 1);
    check("LockDownAllocator, empty vector: nothing assigned", MEM32(ID_SHIP), 7);

    /* 3. the lifted mGetAllocatorID(const char*), which calls both directly */
    registry = make_registry();
    a = guest_string(".?AVtCampaignInfo@@");
    check("mGetAllocatorID(\".?AVtCampaignInfo@@\")", call_guest(VA_GET_ID_BY_NAME, registry, 1, &a), 1);
    check("mGetAllocatorID locked the registry", MEM8(registry), 1);
    check("mGetAllocatorID: tClassAllocator<tBattles> got ID", MEM32(ID_BATTLES), 2);
    a = guest_string(".?AVtNothing@@");
    check("mGetAllocatorID(\".?AVtNothing@@\")", call_guest(VA_GET_ID_BY_NAME, registry, 1, &a),
          0xFFFFFFFFu);

    /* 4. tAllocatedClass::operator==(const type_info&), reached as `call [edx+8]` is */
    static const struct {
        const char *what;
        uint32_t vtable, type, want;
    } eq[] = {
        {"tAllocatedClass object == typeid(tAllocatedClass)", VT_ALLOCATED_CLASS, TD_ALLOCATED_CLASS, 1},
        {"tAllocatedClass object == typeid(tAllocatedClass*)", VT_ALLOCATED_CLASS, TD_ALLOCATED_PTR, 1},
        {"tAllocatedClass object == typeid(tShip)", VT_ALLOCATED_CLASS, TD_SHIP, 0},
        {"tShip object == typeid(tShip)", VT_SHIP, TD_SHIP, 1},
        {"tShip object == typeid(tAllocatedClass)", VT_SHIP, TD_ALLOCATED_CLASS, 0},
        {"tShip object == typeid(tAllocatedClass*)", VT_SHIP, TD_ALLOCATED_PTR, 1},
        {"tShip object == typeid(tCampaignInfo)", VT_SHIP, TD_CAMPAIGN, 0},
    };
    for (unsigned i = 0; i < sizeof eq / sizeof eq[0]; i++) {
        uint32_t object = guest_object(eq[i].vtable, 64);
        uint32_t slot2 = MEM32(MEM32(object) + 8);
        if (i == 0) {
            check("slot 2 of tAllocatedClass's vtable", slot2, VA_ALLOCATED_EQ_TYPE);
        }
        check(eq[i].what, call_guest(slot2, object, 1, &eq[i].type), eq[i].want);
    }

    /* How often each override answered: 0 in the all-lifted build, where there are none. */
    static const uint32_t claimed[3] = {VA_INFO_EQ_NAME, VA_LOCKDOWN, VA_ALLOCATED_EQ_TYPE};
    for (unsigned i = 0; i < 3; i++) {
        printf("[path] 0x%08X override ran %u times\n", claimed[i], sfcop_guest_override_calls(claimed[i]));
    }
    printf("[test] %s\n", g_failed ? "FAILED" : "all checks passed");
    fflush(stdout);
    return g_failed ? 1 : 0;
}
