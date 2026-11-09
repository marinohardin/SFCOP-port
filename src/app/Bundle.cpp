#include "app/Bundle.h"

namespace sfcop::app {

namespace {

// The last component of `path` up to `end` (exclusive), and where it starts.
std::string componentBefore(const std::string &path, std::size_t end, std::size_t &start) {
    const std::size_t slash = end == 0 ? std::string::npos : path.find_last_of("/\\", end - 1);
    start = slash == std::string::npos ? 0 : slash + 1;
    return path.substr(start, end - start);
}

} // namespace

std::string bundleResourcesDir(const std::string &executable) {
    std::size_t exeStart = 0;
    if (componentBefore(executable, executable.size(), exeStart).empty() || exeStart == 0) {
        return std::string();
    }
    std::size_t macosStart = 0;
    if (componentBefore(executable, exeStart - 1, macosStart) != "MacOS" || macosStart == 0) {
        return std::string();
    }
    std::size_t contentsStart = 0;
    if (componentBefore(executable, macosStart - 1, contentsStart) != "Contents") {
        return std::string();
    }
    return executable.substr(0, macosStart) + "Resources";
}

std::string hashListFor(const std::string &executable, const std::string &builtIn,
                        const FileSystem &files) {
    const std::string resources = bundleResourcesDir(executable);
    if (!resources.empty()) {
        const std::string bundled = joinPath(resources, kBundledHashList);
        if (files.fileSize(bundled)) {
            return bundled;
        }
    }
    return builtIn;
}

} // namespace sfcop::app
