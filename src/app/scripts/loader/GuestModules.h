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
// The modules root is $SFCOP_MISSION_MODULES when it is set, else the folder named in the per-user
// data folder's mission-modules.txt (app/DataDir.h; a run started from Finder has no environment),
// else mods/missions/ under the data folder. The guest host names the data folder at start
// (setMissionModulesDataDir()); the working directory is not used for it, because the game moves
// the process's working directory to its install during boot. The root is found once, at the
// game's first LoadLibraryA, and kept for the run. The install is never read for a module and
// never written.
//
// Threading: every call is made with the guest machine held, so one thread at a time. The native
// loader behind MissionModules() checks and opens the modules under the root ahead, on a host
// thread of its own (missionLibraries(), NativeModuleLoader.h): it reads files and maps libraries
// and never touches guest memory, so the guest sees each load exactly as without it.
#pragma once

#include "app/Files.h"
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
#include <vector>

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
    // is missionModulesRoot() at each call.
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

// Has the dispatch hook in `hook` ask `lookup` (the interpreted scripts, InterpretedModules.h) for
// the addresses no native module owns, before the hook it replaced. Both providers share one hook.
void answerInDispatch(sfcop_guest_fn (*lookup)(std::uint32_t), sfcop_guest_fn (**hook)(std::uint32_t));

// The per-user data folder the modules root is found in (above). The guest host sets it before the
// game runs; an empty path, the default, means the working directory, for programs that never set
// it (the tests, the tools). Called before the game's first LoadLibraryA, on one thread: it is
// read without a lock.
void setMissionModulesDataDir(std::filesystem::path dataDir);

// The modules root as the client finds it (above), in the data folder set above.
std::filesystem::path missionModulesRoot();

// The same rule with its inputs passed in: the value of $SFCOP_MISSION_MODULES (nothing when it is
// unset) and the data folder.
std::filesystem::path missionModulesRootFor(const std::optional<std::string> &fromEnvironment,
                                            const std::string &dataDir,
                                            const app::FileSystem &files);

// The libraries under `root` that LibraryFor() would name, in byte order of their names, which is
// the order the game loads its scripts in at boot: the list the loader opens ahead.
std::vector<std::filesystem::path> missionLibraries(const std::filesystem::path &root);

} // namespace sfcop::scripts
