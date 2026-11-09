// Which platform the client runs on and how it reads the environment, passed in rather than read
// directly so that the platform rules of DESIGN §5.3 are tested on every platform.
#pragma once

#include <functional>
#include <optional>
#include <string>

namespace sfcop::app {

enum class Platform { kMacos, kWindows, kLinux };

// The value of an environment variable in UTF-8; nothing when it is unset.
using EnvLookup = std::function<std::optional<std::string>(const std::string &name)>;

// Defined once per platform, in HostPlatform_<platform>.cpp.
Platform hostPlatform();
EnvLookup hostEnvironment();
// The running program's own file, absolute, in UTF-8; empty when the platform cannot say.
std::string executablePath();

} // namespace sfcop::app
