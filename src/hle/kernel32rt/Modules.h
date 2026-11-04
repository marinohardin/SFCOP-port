// Modules: DLLs the host provides itself, for LoadLibraryA, GetModuleHandleA, GetProcAddress and
// FreeLibrary. A built-in module has a handle and named exports at guest addresses (shims); the
// rows answer every other name through pcrecomp's module table (the exe and the system DLLs).
//
// The game ships two tiny DLLs, OSSpecific95.dll and OSSpecific98.dll, and loads one by its bare
// name for a single export; the guest host registers that export as a built-in (src/app/guest/).
//
// Names match as Windows matches them: without case, with ".dll" assumed when the name has no
// extension, and with any directory ignored. Threading: any thread; behind a lock.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace sfcop::hle::kernel32rt {

class Modules {
public:
    // Adds (or replaces) the built-in module `name` at `handle`, with no exports yet.
    void Add(std::string_view name, std::uint32_t handle);
    // An export of a built-in module: `function` at guest address `address`.
    void AddExport(std::uint32_t handle, std::string_view function, std::uint32_t address);

    // The handle of the built-in module `name`, or std::nullopt.
    std::optional<std::uint32_t> Find(std::string_view name) const;
    bool IsBuiltIn(std::uint32_t handle) const;
    // The address of an export by name, or std::nullopt (no such module or export).
    std::optional<std::uint32_t> Export(std::uint32_t handle, std::string_view function) const;

    // The name as Windows compares it: the file name only, lower case, ".dll" when it has no
    // extension ("C:\\Game\\OSSpecific98" -> "osspecific98.dll").
    static std::string Normalise(std::string_view name);

private:
    mutable std::mutex lock_;
    std::map<std::string, std::uint32_t, std::less<>> handles_;
    std::map<std::uint32_t, std::map<std::string, std::uint32_t, std::less<>>> exports_;
};

} // namespace sfcop::hle::kernel32rt
