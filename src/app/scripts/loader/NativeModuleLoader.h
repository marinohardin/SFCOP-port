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

// The real loader over the given lists.
std::unique_ptr<ModuleLoader> makeNativeModuleLoader(AllowList allowList, AllowList hostRuntime);

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
