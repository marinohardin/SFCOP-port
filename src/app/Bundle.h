// Files the client finds beside its own executable when it runs from an application bundle
// (packaging/macos/, scripts/dist.sh): SFCOP.app/Contents/MacOS/sfcop reads its resources from
// SFCOP.app/Contents/Resources. A build run in place has no bundle and keeps the paths it was
// compiled with.
#pragma once

#include <string>

#include "app/Files.h"

namespace sfcop::app {

// The name of the hash list in a bundle's Resources folder.
inline constexpr const char kBundledHashList[] = "retail_hashes.sha256";

// The Resources folder of the bundle `executable` sits in (<bundle>/Contents/MacOS/<exe>), or
// empty when it is not inside one.
std::string bundleResourcesDir(const std::string &executable);

// The hash list the install is verified against: the bundle's copy when `executable` is inside a
// bundle that carries one, else `builtIn`, the source tree's list the build was configured with.
std::string hashListFor(const std::string &executable, const std::string &builtIn,
                        const FileSystem &files);

} // namespace sfcop::app
