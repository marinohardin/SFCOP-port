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
// Trust. A mod's script is untrusted input. Whatever its bytes, the interpreter does not let it read
// or write host memory outside the guest's 4 GiB, nor call a host function except the game's own
// code and the client rows of the imports its file names, each checked against the allow-list:
// - Every guest access goes through interp::Cpu, which refuses one that runs past the guest's last
//   byte (a multi-byte read or write within its last bytes; a rep movs or stos whose length runs
//   past it; an x87 80-bit load or store). Nothing is read or written; the script stops with one
//   line, its call fails, and so does every later one. Lower addresses wrap inside the guest, as
//   32-bit arithmetic does, and an unmapped page there faults as it would in the game.
// - Code is decoded only inside a loaded script's executable sections; control that goes anywhere
//   else stops the script (Interpreter::stopStray).
// - A call out reaches the game's own code (the lift and its manual hooks, which answer the
//   mission modules too) and the client rows of names on the allow-list: those bound for its
//   imports, and those its GetProcAddress finds. Its LoadLibraryA, LoadLibraryExA and
//   GetModuleHandleA reach only the modules the list names, and its GetProcAddress finds only
//   DLL!Name on the list in a module so reached, or its own exports; anything else answers 0, as
//   the native modules' binding shims do (ModuleContract.md §3). The client is handed the name
//   that was checked, copied out of the script's reach: a module as its bare normalised name, a
//   function as read. A name longer than kLookupNameLimit, empty, or a module name with a path
//   (\, / or :) is refused, never cut. Those two names are the only strings the interpreter or
//   its loader read out of guest memory and the only ones with a length limit: whatever else a
//   script passes to the game's code or to an import (briefing and objective text, names,
//   buffers) stays a pointer into guest memory that the callee reads itself, never copied,
//   measured, cut or changed. Any other row (one read from the game's import table) is
//   an unresolved call: said, eax 0, nothing called.
// - Long loops, recursion and rep scans count back edges and hand the machine to other guest
//   threads as the lift does, so a script that never ends does not keep the game's other threads
//   from running.
// What it cannot prevent: a script can write anywhere in the guest, and so corrupt the game's own
// data or code paths; it can call any of the game's functions with any arguments, and through them
// do whatever the game can; and it can run for ever: the thread that called it waits for ever, and
// when that is the game's main thread, as for a mission's calls, the game stops responding, as it
// would with the original.
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
#include <set>
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

    // Whether `address` is a client row bound for an import of a script loaded so far (each passed
    // the allow-list).
    bool BindsRow(std::uint32_t address) const { return boundRows_.count(address) != 0; }
    // Whether a script's call out may reach the client row at `address`: one bound for its imports,
    // or one its GetProcAddress found. Any other is said once and refused.
    bool AllowsRow(std::uint32_t address);
    // The shim a script's dynamic-lookup import is bound to, at `address`, or null
    // (ModuleContract.md §3, dynamic lookups).
    static interp::HostFunction ShimAt(std::uint32_t address);
    // Whether a script's LoadLibraryA, LoadLibraryExA or GetModuleHandleA may reach `module` (as
    // normalizeModuleName gives it): a module the allow-list names.
    bool ScriptMayLoad(std::string_view module) const;
    // Whether a script's GetProcAddress may find `name` ("#n" for an ordinal) in the module whose
    // handle is `handle`: a script's own exports, or DLL!name on the allow-list for a DLL this
    // handle was given for.
    bool ScriptMayFind(std::uint32_t handle, std::string_view name) const;
    // The longest name a lookup shim reads; a longer one is refused.
    static constexpr std::size_t kLookupNameLimit = 255;
    // The shims: the client's own call for a lookup the rule allows, else 0 (said once each), as
    // the native modules' binding shims answer. `shim` is the import's place among the four;
    // `argumentBytes` the stdcall arguments it pops.
    void LoadForScript(std::uint32_t shim, std::uint32_t argumentBytes);
    void FindForScript();

private:
    struct Module {
        std::string name;
        interp::ScriptImage image;
        unsigned references = 0;
    };

    void Unmap(const Module &module);
    // A guest C string of at most kLookupNameLimit bytes, or nullopt when it is longer or runs
    // past the guest's end: a name is refused whole, never cut.
    std::optional<std::string> Text(std::uint32_t address) const;
    std::string Caller() const; // the script a shim's return address is in
    // Calls the client's row for the shim. With `name`, the row is called on a copy of the
    // shim's frame, below it on the guest stack, whose argument `argument` points to a copy of
    // `name` there, so the client reads exactly the name that was checked.
    bool PassOn(std::uint32_t shim, std::uint32_t argumentBytes = 0, std::uint32_t argument = 0,
                const std::string *name = nullptr);
    void Refuse(const std::string &what, std::uint32_t argumentBytes);

    std::optional<GuestRuntime> runtime_;
    std::optional<interp::Machine> machine_;
    ScriptReader reader_;
    const AllowList &allowList_;
    std::unique_ptr<interp::Interpreter> interpreter_;
    std::map<std::uint32_t, Module> byBase_; // the loaded scripts, by base (their handle)
    std::set<std::uint32_t> boundRows_;     // kept past FreeLibrary: each passed the allow-list
    std::set<std::uint32_t> foundRows_;      // rows a script's GetProcAddress was given
    std::set<std::uint32_t> unresolvedRows_; // other rows asked about, said once each
    std::map<std::uint32_t, std::set<std::string>> handleNames_; // module handles the shims gave, by name
    std::set<std::string> refusedLookups_;   // lookups refused, said once each
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
