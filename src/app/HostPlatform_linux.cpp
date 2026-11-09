#include "app/HostPlatform.h"

#include <cstdlib>
#include <filesystem>
#include <system_error>

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

std::string executablePath() {
    std::error_code error;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::string() : self.string();
}

} // namespace sfcop::app
