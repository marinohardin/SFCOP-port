// Mapping a native mission module on Windows: LoadLibraryExW with the module's own folder first in
// the search order, after every check of ModuleContract.md §5 has passed. Written behind the
// header; proven once the Windows build runs (A5).
#include "app/scripts/loader/NativeModuleLoader.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace sfcop::scripts::native {

void *open(const std::filesystem::path &library, std::string *error) {
    HMODULE module = ::LoadLibraryExW(library.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == nullptr && error != nullptr) {
        *error = "LoadLibrary failed with error " + std::to_string(::GetLastError());
    }
    return reinterpret_cast<void *>(module);
}

void *symbol(void *handle, const char *name) {
    return reinterpret_cast<void *>(::GetProcAddress(static_cast<HMODULE>(handle), name));
}

void close(void *handle) {
    ::FreeLibrary(static_cast<HMODULE>(handle));
}

void *programSymbol(const char *name) { // the client exports its runtime for this
    return reinterpret_cast<void *>(::GetProcAddress(::GetModuleHandleW(nullptr), name));
}

} // namespace sfcop::scripts::native
