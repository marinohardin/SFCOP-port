/*
 * ADVAPI32's registry and event-log rows for the game, in pcrecomp's win32hle shim form
 * (src/hle/README.md), calling the registry module through RegistryBridge.h. Key handles are the
 * module's numbers, given to the game as they are. The Reg* rows answer an error code and leave
 * GetLastError alone, as ADVAPI32 does; the event-log rows set it.
 */
#include <stdint.h>
#include <stdio.h>

#include "hle/kernel/KernelBridge.h"
#include "hle/registry/RegistryBridge.h"
#include "win32hle.h"

#define PATH_CHARS 1024

/* Until the guest runtime attaches the module: the per-user folder the client uses without
 * --data, as the kernel module does. */
static void ensure_attached(void) {
    if (sfcop_hle_registry_attached()) return;
    char user[PATH_CHARS] = "";
    sfcop_hle_default_user_dir(user, sizeof user);
    sfcop_hle_registry_attach(user);
}

static void pass_error(void) {
    uint32_t code = 0;
    if (sfcop_registry_take_error(&code)) hle_set_last_error(code);
}

static void put32(uint32_t va, uint32_t value) { if (va) MEM32(va) = value; }

/* With --trace: the key this module answered, so a trace shows the rows are ours. */
static void trace_key(const char *row, uint32_t parent, const char *name, uint32_t result) {
    if (win32hle_trace)
        fprintf(stderr, "[hle-registry] %s 0x%x \"%s\" -> %u\n", row, parent, name ? name : "", result);
}

static void r_RegOpenKeyExA(void) {             /* (key, subKey, options, access, &result) */
    ensure_attached();
    uint32_t key = 0;
    uint32_t error = sfcop_registry_open_key(A32(0), ASTR(1), &key);
    put32(A32(4), key);
    trace_key("RegOpenKeyExA", A32(0), ASTR(1), error);
    RET(error, 5);
}

/* (key, subKey, reserved, class, options, access, security, &result, &disposition) */
static void r_RegCreateKeyExA(void) {
    ensure_attached();
    uint32_t key = 0, disposition = 0;
    uint32_t error = sfcop_registry_create_key(A32(0), ASTR(1), &key, &disposition);
    put32(A32(7), key);
    if (error == 0) put32(A32(8), disposition);
    trace_key("RegCreateKeyExA", A32(0), ASTR(1), error);
    RET(error, 9);
}

static void r_RegCloseKey(void) {               /* (key) */
    ensure_attached();
    RET(sfcop_registry_close_key(A32(0)), 1);
}

static void r_RegDeleteKeyA(void) {             /* (key, subKey) */
    ensure_attached();
    uint32_t error = sfcop_registry_delete_key(A32(0), ASTR(1));
    trace_key("RegDeleteKeyA", A32(0), ASTR(1), error);
    RET(error, 2);
}

static void r_RegQueryValueExA(void) {          /* (key, name, reserved, &type, data, &size) */
    ensure_attached();
    uint32_t typeVa = A32(3), sizeVa = A32(5);
    uint32_t type = 0, size = sizeVa ? MEM32(sizeVa) : 0;
    uint32_t error = sfcop_registry_query_value(A32(0), ASTR(1), typeVa ? &type : NULL,
                                                (uint8_t *)APTR(4), sizeVa ? &size : NULL);
    if (error == 0 || error == 234) {           /* ERROR_MORE_DATA still gives type and size */
        put32(typeVa, type);
        put32(sizeVa, size);
    }
    trace_key("RegQueryValueExA", A32(0), ASTR(1), error);
    RET(error, 6);
}

static void r_RegSetValueExA(void) {            /* (key, name, reserved, type, data, size) */
    ensure_attached();
    uint32_t error = sfcop_registry_set_value(A32(0), ASTR(1), A32(3), (const uint8_t *)APTR(4), A32(5));
    trace_key("RegSetValueExA", A32(0), ASTR(1), error);
    RET(error, 6);
}

static void r_RegisterEventSourceA(void) {      /* (server, source) */
    ensure_attached();
    uint32_t handle = sfcop_registry_register_event_source(ASTR(0), ASTR(1));
    pass_error();
    RET(handle, 2);
}

/* (source, type, category, id, user, stringCount, dataSize, strings, data): accepted, not kept. */
static void r_ReportEventA(void) {
    ensure_attached();
    int ok = sfcop_registry_report_event(A32(0));
    pass_error();
    RET(ok, 9);
}

static void r_DeregisterEventSource(void) {     /* (source) */
    ensure_attached();
    int ok = sfcop_registry_deregister_event_source(A32(0));
    pass_error();
    RET(ok, 1);
}

const win32hle_shim sfcop_hle_registry[] = {
    { "DeregisterEventSource", r_DeregisterEventSource },
    { "RegCloseKey", r_RegCloseKey },
    { "RegCreateKeyExA", r_RegCreateKeyExA },
    { "RegDeleteKeyA", r_RegDeleteKeyA },
    { "RegisterEventSourceA", r_RegisterEventSourceA },
    { "RegOpenKeyExA", r_RegOpenKeyExA },
    { "RegQueryValueExA", r_RegQueryValueExA },
    { "RegSetValueExA", r_RegSetValueExA },
    { "ReportEventA", r_ReportEventA },
    { 0, 0 }
};
