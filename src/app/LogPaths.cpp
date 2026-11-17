#include "LogPaths.h"

#include <cstdlib>

namespace sfcop::app {

namespace {

bool isSeparator(char c) { return c == '/' || c == '\\'; }

} // namespace

std::string collapseHome(const std::string &path, const std::string &home) {
    std::string base = home;
    while (!base.empty() && isSeparator(base.back())) {
        base.pop_back();
    }
    if (base.empty() || path.compare(0, base.size(), base) != 0) {
        return path;
    }
    if (path.size() == base.size()) {
        return "~";
    }
    return isSeparator(path[base.size()]) ? "~" + path.substr(base.size()) : path;
}

std::string collapseHome(const std::string &path) {
#ifdef _WIN32
    const char *home = std::getenv("USERPROFILE");
#else
    const char *home = std::getenv("HOME");
#endif
    return collapseHome(path, home != nullptr ? home : "");
}

} // namespace sfcop::app
