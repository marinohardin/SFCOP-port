// Mapping a native mission module on Linux: dlopen, after every check of ModuleContract.md §5 has
// passed. RTLD_LOCAL keeps one module's symbols from binding another's. Written behind the header;
// proven once the Linux build runs (A5).
#include "app/scripts/loader/NativeModuleLoader.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE // RTLD_DEFAULT
#endif
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
    return ::dlsym(RTLD_DEFAULT, name); // the client links with -rdynamic for this
}

} // namespace sfcop::scripts::native
