#include <stdlib.h>
#define EXPORT __attribute__((visibility("default")))
struct tag { char magic[8]; unsigned contract; float script; char abi[32]; };
EXPORT const struct tag sfcop_module_abi = {{'S','F','C','O','P','M','O','D'}, 1u, 22.0f, "arm64-macos-itanium"};
EXPORT const void *MissionSpecEntry(float v) { return v == 22.0f ? (const void *)0x1234 : 0; }
EXPORT void *ScriptEntry(void) { return (void *)0x5678; }
