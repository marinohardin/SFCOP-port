// The real ModuleLoader (ModuleContract.md §5, §6): a module directory read into a ModuleImage from
// its files alone, checked with checkImage(), then mapped by the platform's loader.
//
// The readers parse bytes and run on every host, so a Mach-O, ELF or PE file is refused with its
// reason wherever it is read; only mapping is per platform (ModuleLoader_<platform>.cpp).
#pragma once

#include "app/scripts/ModuleLoader.h"

#include <memory>
#include <string>
#include <string_view>

namespace sfcop::scripts {

// What a shared library's file says, for `hostAbi` (kHostAbiTag by default). `notLoadable` is set
// when the file is not a dynamic library of that host's format and architecture.
struct LibraryInfo {
    std::string notLoadable;
    std::optional<ModuleAbiTag> abiTag;
    std::vector<std::string> undefinedSymbols; // the object format's prefix removed, in file order
    std::vector<std::string> exportedSymbols;
};
LibraryInfo readLibrary(std::string_view bytes, std::string_view hostAbi = kHostAbiTag);

// The same, from the file, reading only the parts the readers look at: its headers, symbol table
// and strings, about 150 KB of a module's 9 MB. `bytesRead`, when given, counts what was read.
// A file that cannot be opened is not loadable: "cannot be read".
LibraryInfo readLibraryFile(const std::filesystem::path &library, std::string_view hostAbi = kHostAbiTag,
                            std::uint64_t *bytesRead = nullptr);

// module.json (ModuleContract.md §4): the manifest, or the invalid field in `error`.
struct ManifestRead {
    std::optional<ModuleManifest> manifest;
    std::string error;
};
ManifestRead parseManifest(std::string_view json);

// The image checkImage() reads, from `library` and the module.json beside it.
ModuleImage readModuleImage(const std::filesystem::path &library);

// The allow-list and this host's baseline list, as the build embedded them (allowlist.txt and
// baseline-<kHostAbiTag>.txt beside ModuleLoader.h). A host with no baseline list gets an empty
// one, and every host-runtime import is refused there.
const AllowList &builtInAllowList();
const AllowList &builtInHostBaseline();

// The real loader over the given lists. The libraries in `openAhead` are checked and, when the
// checks pass, opened by the platform's loader on a thread of their own, in that order, from now
// on (OpenAhead.h); load() of one of them then takes what that thread did, or waits for it, instead
// of doing it. Nothing else changes: each is still checked before it is opened, refused with the
// same reason, opened once per load and closed by unload(). A library opened ahead and never
// loaded is closed with the loader. A thrown exception reaches load()'s caller, as without it. A
// library with initializers would run them on that thread, when it is opened; scr2native's modules
// have none today (no initializer section in any of the 89), but nothing here checks that.
std::unique_ptr<ModuleLoader> makeNativeModuleLoader(AllowList allowList, AllowList hostRuntime,
                                                     const std::vector<std::filesystem::path> &openAhead = {});

// The reason a refusal gives when the platform's loader fails: the library's file name and the
// message after the last colon of the loader's own text, which names the full path of the library
// and of every place it looked. `loaderText` is dlerror()'s, or the platform's equivalent.
std::string loaderFailureReason(const std::filesystem::path &library, std::string_view loaderText);

// The platform's loader (ModuleLoader_<platform>.cpp).
namespace native {
void *open(const std::filesystem::path &library, std::string *error);
void *symbol(void *handle, const char *name);
void close(void *handle);
// A symbol of the running program itself (the lifted client's runtime), or null.
void *programSymbol(const char *name);
} // namespace native

} // namespace sfcop::scripts
