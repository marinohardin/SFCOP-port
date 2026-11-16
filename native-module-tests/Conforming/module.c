#include <stdlib.h>
#define EXPORT __attribute__((visibility("default")))
struct tag { char magic[8]; unsigned contract; float script; char abi[32]; };
EXPORT const struct tag sfcop_module_abi = {{'S','F','C','O','P','M','O','D'}, 1u, 22.0f, "arm64-macos-itanium"};
EXPORT const void *MissionSpecEntry(float v) { return v == 22.0f ? (const void *)0x1234 : 0; }
EXPORT void *ScriptEntry(void) { return (void *)0x5678; }
extern char import0 __asm__("_sfcop_import:??0tShipInfo@@QAE@XZ") __attribute__((weak_import));
EXPORT void *ref0(void) { return &import0; }
extern char import1 __asm__("_sfcop_import:?mAssessDamage@tShipInfo@@QAEJW4eRace@@@Z") __attribute__((weak_import));
EXPORT void *ref1(void) { return &import1; }
extern char import2 __asm__("_sfcop_import:?mAddShipToTeamList@tTeamInfo@@QAEEAAUtCreateShipInfo@@@Z") __attribute__((weak_import));
EXPORT void *ref2(void) { return &import2; }
extern char import3 __asm__("_sfcop_import:??0tString@@QAE@PBD@Z") __attribute__((weak_import));
EXPORT void *ref3(void) { return &import3; }
extern char import4 __asm__("_sfcop_import:?mIdentity@tMatrix@@QAEXXZ") __attribute__((weak_import));
EXPORT void *ref4(void) { return &import4; }
extern char import5 __asm__("_sfcop_import:_atexit") __attribute__((weak_import));
EXPORT void *ref5(void) { return &import5; }
extern char import6 __asm__("_sfcop_import:_free") __attribute__((weak_import));
EXPORT void *ref6(void) { return &import6; }
extern char import7 __asm__("_sfcop_import:__CxxThrowException@8") __attribute__((weak_import));
EXPORT void *ref7(void) { return &import7; }
extern char import8 __asm__("_sfcop_import:KERNEL32.DLL!HeapAlloc") __attribute__((weak_import));
EXPORT void *ref8(void) { return &import8; }
extern char import9 __asm__("_sfcop_import:USER32.DLL!wsprintfA") __attribute__((weak_import));
EXPORT void *ref9(void) { return &import9; }
