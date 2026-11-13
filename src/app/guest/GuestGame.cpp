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
//    place, answered by the manual lookup hook, which indirect calls ask only for addresses the
//    dispatch table lacks.
// 3. The override table (override.h) is written into the dispatch table at start-up
//    (hle_dispatch_fold), so an indirect call to an overridden address finds the override in the
//    table, cached, instead of searching the override table first on every call; from the fold
//    on, indirect calls ask the table before the hook (recomp_types.h). Then every
//    address the table, the override table and the hook answer is checked against the answer the
//    old lookup order gave, and a difference stops the run.
// 4. The marks the lift puts at the entry of WinMain, of the CD-key prompt, of the main menu
//    ("Select Game", tState_SelectGame) and of Setup (tState_Setup, the Single Player screen one
//    click further); reaching the prompt or the menu is what a --watchdog run counts as success.
#include "GuestHost.h"
#include "hle/kernel32rt/Runtime.h"
#include "app/guest/override.h"
#include "app/input/Marks.h"

extern "C" {
#include "win32hle.h"

extern recomp_func_t (*hle_manual_hook)(uint32_t va);
void sub_004014A9(void);
}

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

namespace sfcop::guest {

namespace {

constexpr std::uint32_t kOsSpecificModule = 0x7FF00000u;
constexpr std::uint32_t kMissedConstructor = 0x0059644Au;
constexpr std::uint32_t kWinMain = SFCOP_MARK_WINMAIN;
constexpr std::uint32_t kCdKeyPrompt = SFCOP_MARK_CDKEY_PROMPT;
constexpr std::uint32_t kMenu = SFCOP_MARK_MENU;
constexpr std::uint32_t kSetup = SFCOP_MARK_SETUP;

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
    return address == kMissedConstructor ? sub_004014A9 : nullptr;
}

// The generated dispatch table, as recomp_lookup searched it before the overrides were folded in.
recomp_func_t generatedLookup(std::uint32_t address) {
    const recomp_dispatch_entry_t *first = recomp_dispatch_table;
    const recomp_dispatch_entry_t *last = first + recomp_dispatch_count;
    const auto at = std::lower_bound(first, last, address,
                                     [](const recomp_dispatch_entry_t &e, std::uint32_t a) {
                                         return e.address < a;
                                     });
    return at != last && at->address == address ? at->func : nullptr;
}

// What an indirect call found before the fold: the override table, the missed constructor, then
// the generated table.
recomp_func_t lookupBefore(std::uint32_t address) {
    if (recomp_func_t replaced = sfcop_guest_override_lookup(address)) {
        return replaced;
    }
    if (recomp_func_t manual = manualLookup(address)) {
        return manual;
    }
    return generatedLookup(address);
}

// What an indirect call finds now: the dispatch table (cached), then the manual hook.
recomp_func_t lookupAfter(std::uint32_t address) {
    if (recomp_func_t found = recomp_lookup(address)) {
        return found;
    }
    return manualLookup(address);
}

// Every address with an answer, asked twice (the second answer comes from recomp_lookup's
// cache). Returns the first address whose answer changed, or 0.
std::uint32_t firstChangedAddress() {
    std::vector<std::uint32_t> addresses;
    addresses.reserve(recomp_dispatch_count + sfcop_guest_override_count + 1);
    for (std::uint32_t i = 0; i < recomp_dispatch_count; i++) {
        addresses.push_back(recomp_dispatch_table[i].address);
    }
    for (unsigned i = 0; i < sfcop_guest_override_count; i++) {
        addresses.push_back(sfcop_guest_overrides[i].va);
    }
    addresses.push_back(kMissedConstructor);
    for (int pass = 0; pass < 2; pass++) {
        for (const std::uint32_t address : addresses) {
            if (lookupAfter(address) != lookupBefore(address)) {
                return address;
            }
        }
    }
    return 0;
}

void foldOverrides() {
    std::vector<recomp_dispatch_entry_t> rows;
    rows.reserve(sfcop_guest_override_count);
    for (unsigned i = 0; i < sfcop_guest_override_count; i++) {
        rows.push_back({sfcop_guest_overrides[i].va, sfcop_guest_overrides[i].entry});
    }
    const auto start = std::chrono::steady_clock::now();
    const int added = hle_dispatch_fold(rows.data(), static_cast<std::uint32_t>(rows.size()));
    if (const std::uint32_t changed = firstChangedAddress()) {
        hle_fatal("[dispatch] 0x%08X resolves to another function after the overrides were "
                  "folded in",
                  changed);
    }
    const std::chrono::duration<double, std::milli> took = std::chrono::steady_clock::now() - start;
    std::fprintf(stderr,
                 "[dispatch] %u overrides folded into %u entries (%d added); every address "
                 "resolves as before (%.1f ms)\n",
                 sfcop_guest_override_count, recomp_dispatch_count, added, took.count());
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
    foldOverrides();
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
            const char *name = address == sfcop::guest::kWinMain        ? "WinMain"
                               : address == sfcop::guest::kCdKeyPrompt ? "CD-key prompt"
                               : address == sfcop::guest::kMenu        ? "Select Game"
                               : address == sfcop::guest::kSetup       ? "Setup"
                                                                       : "";
            std::fprintf(stderr, "[mark] reached 0x%08X%s%s%s at %.3f s\n", address,
                         *name ? " (" : "", name, *name ? ")" : "", sfcop::guest::Elapsed());
            sfcop::app::input::NoteMark(address, name);
            if (address == sfcop::guest::kCdKeyPrompt || address == sfcop::guest::kMenu) {
                sfcop::guest::ReachedGoal();
            }
            return;
        }
    }
}
