// The game's mission scripts, answered by translated native modules (ModuleContract.md §1, §6;
// DESIGN §5.9 path (b)). When the game calls LoadLibraryA for a ".scr", the KERNEL32 rows ask this
// provider (hle/kernel32rt Modules.h): it loads <modules root>/<Name>/<Name>.<dylib|so|dll> through
// the real ModuleLoader, which checks it before any of its code runs, copies the module's guest
// image into the game's address space at the module's base, binds its Windows imports to the
// client's rows and its machine to the client's, answers its guest addresses in the lifter's
// dispatch, and runs its DLL entry point. GetProcAddress gives the guest addresses of its exports.
//
// A module that cannot be loaded is logged with the loader's refusal message and LoadLibraryA
// returns 0, as Windows does for a DLL that fails to load; the game goes on without that mission.
//
// The modules root is $SFCOP_MISSION_MODULES when it is set, else mods/missions/ under the working
// directory, which the guest host makes the per-user data folder. The install is never read for a
// module and never written.
//
// Threading: every call is made with the guest machine held, so one thread at a time.
#pragma once

#include "app/scripts/ModuleLoader.h"
#include "app/scripts/guest/GuestModuleAbi.h"
#include "hle/kernel32rt/Modules.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace sfcop::scripts {

// What the provider needs from the running client, so that tests can stand in for it.
struct GuestRuntime {
    // Reserves [address, address + size) of guest memory, zeroed; false when any of it is in use.
    std::function<bool(std::uint32_t address, std::uint32_t size)> reserve;
    std::function<void(std::uint32_t address, std::uint32_t size)> release;
    // The host address of a guest address inside a reservation.
    std::function<std::uint8_t *(std::uint32_t address)> memory;
    // The guest address of the client's row for a Windows import ("KERNEL32.DLL!HeapAlloc"). Never
    // 0: a row nobody answers is a stub that ends the run, naming itself, when it is called.
    std::function<std::uint32_t(const std::string &import)> importAddress;
    // The client's runtime by name, for a module's bind().
    sfcop_guest_resolver symbol = nullptr;
    // The lifter's dispatch hook for addresses outside the game (pcrecomp's hle_manual_hook); the
    // provider answers its modules' addresses there and passes every other address on.
    sfcop_guest_fn (**lookupHook)(std::uint32_t address) = nullptr;
};

// The runtime of the client this code runs in, found by name in the running program; std::nullopt
// when the program is not the lifted client (the tests, a build without the lift).
std::optional<GuestRuntime> clientRuntime();

class GuestModuleProvider final : public hle::kernel32rt::ModuleProvider {
public:
    GuestModuleProvider(ModuleLoader &loader, std::optional<GuestRuntime> runtime,
                        std::filesystem::path root, const AllowList &allowList);
    ~GuestModuleProvider() override;
    GuestModuleProvider(const GuestModuleProvider &) = delete;
    GuestModuleProvider &operator=(const GuestModuleProvider &) = delete;

    bool Handles(std::string_view name) const override;
    std::uint32_t Load(std::string_view name) override;
    bool Owns(std::uint32_t handle) const override;
    std::uint32_t Export(std::uint32_t handle, std::string_view function) override;
    void Free(std::uint32_t handle) override;

    // The lifted function of a loaded module at a guest address, or null: the dispatch's answer for
    // addresses inside a module, and only those.
    sfcop_guest_fn Lookup(std::uint32_t address) const;

    // Where the module of a script name lives: <root>/<Name>/<Name><library suffix>. An empty root
    // is missionModulesRoot(), found at the first load, once the host has set the working directory.
    std::filesystem::path LibraryFor(std::string_view scriptName) const;

private:
    struct Module {
        std::string name;
        ModuleId id = 0;
        const sfcop_guest_image *image = nullptr;
        unsigned references = 0;
    };

    // Why the image cannot be mapped, or empty: its version, its range, its dispatch and its
    // imports are checked before anything is written to guest memory.
    std::string CheckImage(const std::string &name, const sfcop_guest_image &image) const;
    void Unmap(const Module &module);
    void InstallHook();

    ModuleLoader &loader_;
    std::optional<GuestRuntime> runtime_;
    std::filesystem::path root_;
    const AllowList &allowList_;
    std::map<std::uint32_t, Module> byBase_; // the loaded modules, by base (their handle)
};

// The modules root as the client finds it (above).
std::filesystem::path missionModulesRoot();

} // namespace sfcop::scripts
