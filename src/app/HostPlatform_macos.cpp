#include "app/HostPlatform.h"

#include <climits>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include <mach-o/dyld.h>

namespace sfcop::app {

Platform hostPlatform() { return Platform::kMacos; }

EnvLookup hostEnvironment() {
    return [](const std::string &name) -> std::optional<std::string> {
        const char *value = std::getenv(name.c_str());
        if (value == nullptr) {
            return std::nullopt;
        }
        return std::string(value);
    };
}

std::string executablePath() {
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> path(size + 1, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) {
        return std::string();
    }
    char resolved[PATH_MAX];
    if (realpath(path.data(), resolved) == nullptr) {
        return std::string(path.data());
    }
    return std::string(resolved);
}

} // namespace sfcop::app
