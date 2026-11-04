#include "app/HostPlatform.h"

#include <cstdlib>

namespace sfcop::app {

Platform hostPlatform() { return Platform::kLinux; }

EnvLookup hostEnvironment() {
    return [](const std::string &name) -> std::optional<std::string> {
        const char *value = std::getenv(name.c_str());
        if (value == nullptr) {
            return std::nullopt;
        }
        return std::string(value);
    };
}

} // namespace sfcop::app
