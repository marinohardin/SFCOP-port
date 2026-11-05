// Mapping a native mission module on macOS: dlopen, after every check of ModuleContract.md §5 has
// passed. RTLD_LOCAL keeps one module's symbols from binding another's.
#include "app/scripts/loader/NativeModuleLoader.h"

#include <dlfcn.h>

namespace sfcop::scripts::native {

void *open(const std::filesystem::path &library, std::string *error) {
    void *handle = ::dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr && error != nullptr) {
        const char *why = ::dlerror();
        *error = why != nullptr ? why : "dlopen failed";
    }
    return handle;
}

void *symbol(void *handle, const char *name) {
    return ::dlsym(handle, name);
}

void close(void *handle) {
    ::dlclose(handle);
}

void *programSymbol(const char *name) {
    return ::dlsym(RTLD_DEFAULT, name);
}

} // namespace sfcop::scripts::native
