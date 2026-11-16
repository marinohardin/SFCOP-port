#include <stdlib.h>
#define EXPORT __attribute__((visibility("default")))
EXPORT const void *MissionSpecEntry(float v) { return v == 22.0f ? (const void *)0x1234 : 0; }
EXPORT void *ScriptEntry(void) { return (void *)0x5678; }
