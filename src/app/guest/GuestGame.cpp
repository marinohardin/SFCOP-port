// What only StarFleetOP.exe needs from the guest host (GuestHost.h, InstallGameSpecifics).
//
// 1. OSSpecific95.dll and OSSpecific98.dll. A static constructor (0x006F54E0) loads one of them
//    by its bare name and takes its single export; without it the fallback it installs throws on
//    first use. The DLL holds one function, so the host is the DLL: a built-in module (hle/
//    kernel32rt Modules.h) whose export is cdecl OS_InterlockedCompareExchange(dest, exchange,
//    comparand).
// 2. The static constructor at 0x0059644A. It is reachable only through the C runtime's _initterm
//    table, so the lift has no function there. Its bytes do what 0x004014A9's do, constant for
//    constant (set bit 0 of the guard at 0x00C8193C, then atexit(0x004BD431)): one of the
//    duplicate initialisers MSVC emits for a guarded static. The lifted 0x004014A9 runs in its
//    place. B9's override table (override.h) is asked first.
// 3. The marks the lift puts at the entry of WinMain and of the CD-key prompt; reaching the
//    prompt is what a --watchdog run counts as success.
#include "GuestHost.h"
#include "hle/kernel32rt/Runtime.h"
#include "app/guest/override.h"

extern "C" {
#include "win32hle.h"

extern recomp_func_t (*hle_manual_hook)(uint32_t va);
void sub_004014A9(void);
}

#include <cstdio>

namespace sfcop::guest {

namespace {

constexpr std::uint32_t kOsSpecificModule = 0x7FF00000u;
constexpr std::uint32_t kMissedConstructor = 0x0059644Au;
constexpr std::uint32_t kWinMain = SFCOP_MARK_WINMAIN;
constexpr std::uint32_t kCdKeyPrompt = SFCOP_MARK_CDKEY_PROMPT;

void osInterlockedCompareExchange() { // cdecl (destination, exchange, comparand)
    const std::uint32_t destination = A32(0), exchange = A32(1), comparand = A32(2);
    const std::uint32_t previous = MEM32(destination);
    if (previous == comparand) {
        MEM32(destination) = exchange;
    }
    RET(previous, 0);
}

const win32hle_shim kOsSpecificExports[] = {
    {"OSSpecific.OS_InterlockedCompareExchange", osInterlockedCompareExchange},
    {nullptr, nullptr},
};

recomp_func_t manualLookup(std::uint32_t address) {
    if (recomp_func_t replaced = sfcop_guest_override_lookup(address)) {
        return replaced;
    }
    return address == kMissedConstructor ? sub_004014A9 : nullptr;
}

} // namespace

void InstallGameSpecifics() {
    win32hle_register(kOsSpecificExports);
    auto &modules = hle::kernel32rt::Runtime::Instance().BuiltIns();
    const std::uint32_t export_ = hle_resolve(kOsSpecificExports[0].name);
    for (const char *name : {"OSSpecific95.dll", "OSSpecific98.dll"}) {
        modules.Add(name, kOsSpecificModule);
    }
    modules.AddExport(kOsSpecificModule, "OS_InterlockedCompareExchange", export_);
    hle_manual_hook = manualLookup;
}

} // namespace sfcop::guest

// Called on entry of each function the lift was told to mark (cmake/SfcopLifter.cmake).
extern "C" void sfcop_mark(uint32_t address) {
    static std::uint32_t seen[16];
    for (std::uint32_t &slot : seen) {
        if (slot == address) {
            return;
        }
        if (slot == 0) {
            slot = address;
            std::fprintf(stderr, "[mark] reached 0x%08X%s at %.3f s\n", address,
                         address == sfcop::guest::kWinMain        ? " (WinMain)"
                         : address == sfcop::guest::kCdKeyPrompt ? " (CD-key prompt)"
                                                                 : "",
                         sfcop::guest::Elapsed());
            if (address == sfcop::guest::kCdKeyPrompt) {
                sfcop::guest::ReachedGoal();
            }
            return;
        }
    }
}
