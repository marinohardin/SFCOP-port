// Files the client finds beside its own executable when it runs from a package (packaging/,
// scripts/dist.sh): SFCOP.app/Contents/MacOS/sfcop reads its resources from
// SFCOP.app/Contents/Resources; the Linux and Windows packages, which have no bundle, keep them in
// the executable's own folder. A build run in place has neither and keeps the paths it was
// compiled with.
#pragma once

#include <string>
#include <vector>

#include "app/Files.h"

namespace sfcop::app {

// The name of the hash list in a bundle's Resources folder.
inline constexpr const char kBundledHashList[] = "retail_hashes.sha256";

// The Resources folder of the bundle `executable` sits in (<bundle>/Contents/MacOS/<exe>), or
// empty when it is not inside one.
std::string bundleResourcesDir(const std::string &executable);

// The hash list the install is verified against: the bundle's copy when `executable` is inside a
// bundle that carries one, else the copy in the executable's own folder when there is one (the
// Linux and Windows packages, because a relocated package cannot read the build machine's source
// tree), else `builtIn`, the source tree's list the build was configured with. Empty when none of
// these exists: a package build (SFCOP_PACKAGE_BUILD) compiles in no list of its own.
std::string hashListFor(const std::string &executable, const std::string &builtIn,
                        const FileSystem &files);

// The install candidates the build gives: `builtIn` alone, or none when it is empty (a package
// build gives none, and the client asks).
std::vector<std::string> builtInInstallDirs(const std::string &builtIn);

} // namespace sfcop::app
