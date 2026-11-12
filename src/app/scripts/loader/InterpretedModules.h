// The game's mission scripts run from the player's own .scr files (DESIGN §5.9), for every script
// that has no native module (GuestModules.h): when the game calls LoadLibraryA for a ".scr", this
// provider reads the file the game names, lays its image out at the script's slot, checks every
// import against the allow-list before any of it is written to guest memory, binds the imports to
// the client's rows, and runs the DLL entry point on the x86-32 interpreter (interp/). The game
// then enters script code through the lifter's dispatch, which this provider answers for every
// address in a script's code, and the script calls back out through the same dispatch.
//
// A script's slot is the one scr2native gives its module: 0x60000000 + 1 MB x n, where n is the
// script's place among its folder's .scr files in byte order of their names. A script that is not
// among the first 256, or whose image is larger than a slot, is refused.
//
// A script that cannot be loaded is logged with one sentence and LoadLibraryA returns 0.
//
// Threading: every call is made with the guest machine held, so one thread at a time.
#pragma once

#include "app/scripts/ModuleLoader.h"
#include "app/scripts/interp/Interpreter.h"
#include "app/scripts/interp/ScriptImage.h"
#include "app/scripts/loader/GuestModules.h"
#include "hle/kernel32rt/Modules.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sfcop::scripts {

// The bytes of the script the game names and its slot's base.
struct ScriptFile {
    std::vector<std::uint8_t> bytes;
    std::uint32_t base = 0;
};
// Reads the script the game names (its LoadLibraryA argument). std::nullopt, with `why` one
// sentence, when it cannot.
using ScriptReader = std::function<std::optional<ScriptFile>(std::string_view name, std::string &why)>;

// The client's reader: the file through the client's file system (the install's folders as the
// game sees them), the slot from a listing of its folder.
std::optional<ScriptFile> readGuestScript(std::string_view name, std::string &why);

// The base of slot `index` (0x60000000 + 1 MB x index), or 0 past the 256th.
std::uint32_t scriptSlotBase(std::size_t index);
// The slot of `fileName` among `folder` (the .scr file names of its folder; other names are
// skipped): its place in byte order, the name matched without case as Windows matches it, or
// std::nullopt when it is not there.
std::optional<std::size_t> scriptSlotIndex(std::string_view fileName, std::vector<std::string> folder);

// The client's machine: the lifted game's registers, flags and runtime, by name in the running
// program; std::nullopt when the program is not the lifted client.
std::optional<interp::Machine> clientMachine();

class InterpretedModuleProvider final : public hle::kernel32rt::ModuleProvider {
public:
    InterpretedModuleProvider(std::optional<GuestRuntime> runtime, std::optional<interp::Machine> machine,
                              ScriptReader reader, const AllowList &allowList);
    ~InterpretedModuleProvider() override;
    InterpretedModuleProvider(const InterpretedModuleProvider &) = delete;
    InterpretedModuleProvider &operator=(const InterpretedModuleProvider &) = delete;

    bool Handles(std::string_view name) const override;
    std::uint32_t Load(std::string_view name) override;
    bool Owns(std::uint32_t handle) const override;
    std::uint32_t Export(std::uint32_t handle, std::string_view function) override;
    void Free(std::uint32_t handle) override;

    // The function that enters script code at a guest address, or null: the dispatch's answer for
    // addresses in a loaded script's code, and only those.
    sfcop_guest_fn Lookup(std::uint32_t address);

    // Calls the script function at `address` as lifted code would: the arguments pushed right to
    // left, the lifter's return address, then the function; the result is eax. The machine's
    // registers are as they were afterwards.
    std::uint32_t Call(std::uint32_t address, std::uint32_t argc, const std::uint32_t *args);

    const interp::InterpreterStats &Stats() const { return interpreter_->stats(); }

private:
    struct Module {
        std::string name;
        interp::ScriptImage image;
        unsigned references = 0;
    };

    void Unmap(const Module &module);

    std::optional<GuestRuntime> runtime_;
    std::optional<interp::Machine> machine_;
    ScriptReader reader_;
    const AllowList &allowList_;
    std::unique_ptr<interp::Interpreter> interpreter_;
    std::map<std::uint32_t, Module> byBase_; // the loaded scripts, by base (their handle)
};

// The provider the client answers ".scr" names with: a script's native module when there is a file
// for it under the modules root, else the script itself, interpreted. A native module that is
// there but refused is logged, and the script is interpreted.
class MissionModuleProvider final : public hle::kernel32rt::ModuleProvider {
public:
    MissionModuleProvider(GuestModuleProvider &native, InterpretedModuleProvider &interpreted);

    bool Handles(std::string_view name) const override;
    std::uint32_t Load(std::string_view name) override;
    bool Owns(std::uint32_t handle) const override;
    std::uint32_t Export(std::uint32_t handle, std::string_view function) override;
    void Free(std::uint32_t handle) override;

private:
    hle::kernel32rt::ModuleProvider &Owner(std::uint32_t handle);

    GuestModuleProvider &native_;
    InterpretedModuleProvider &interpreted_;
};

} // namespace sfcop::scripts
