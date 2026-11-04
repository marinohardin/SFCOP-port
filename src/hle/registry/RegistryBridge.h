/*
 * The registry module as RegistryGuest.c calls it: plain C, for the same reason as
 * hle/kernel/KernelBridge.h. Key handles are the module's own numbers and go to the game as they
 * are; out-parameters are the shim's locals, which it writes back into guest memory. The Reg*
 * calls answer a Win32 error code; the event-log calls a handle or BOOL, with the error in
 * sfcop_registry_take_error.
 *
 * One registry serves the process, behind a lock, attached as the kernel module is.
 */
#ifndef SFCOP_HLE_REGISTRY_BRIDGE_H
#define SFCOP_HLE_REGISTRY_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The per-user folder (absolute host path, UTF-8) that holds registry.tsv; created when missing.
 * Replaces any earlier attachment and forgets the key handles it gave out. */
void sfcop_hle_registry_attach(const char *userDir);
void sfcop_hle_registry_detach(void);
int sfcop_hle_registry_attached(void);

int sfcop_registry_take_error(uint32_t *code);

uint32_t sfcop_registry_open_key(uint32_t parent, const char *subKey, uint32_t *result);
uint32_t sfcop_registry_create_key(uint32_t parent, const char *subKey, uint32_t *result,
                                   uint32_t *disposition);
uint32_t sfcop_registry_close_key(uint32_t key);
uint32_t sfcop_registry_delete_key(uint32_t parent, const char *subKey);
/* `type` and `size` may be NULL; `data` is the game's buffer of *size bytes, or NULL. */
uint32_t sfcop_registry_query_value(uint32_t key, const char *name, uint32_t *type, uint8_t *data,
                                    uint32_t *size);
uint32_t sfcop_registry_set_value(uint32_t key, const char *name, uint32_t type,
                                  const uint8_t *data, uint32_t size);

uint32_t sfcop_registry_register_event_source(const char *server, const char *source);
int sfcop_registry_deregister_event_source(uint32_t handle);
int sfcop_registry_report_event(uint32_t handle);

#ifdef __cplusplus
}
#endif

#endif /* SFCOP_HLE_REGISTRY_BRIDGE_H */
