// A path as it is written to a log: the home folder replaced by `~`, so a log carries no account
// name (DESIGN §5.16). A path outside the home folder is left as it is.
#pragma once

#include <string>

namespace sfcop::app {

// `path` with the leading `home` replaced by `~` when `home` is a whole leading folder of it (the
// path equals it, or continues with a separator); otherwise `path` unchanged. A trailing separator
// on `home` is ignored, and an empty or root `home` changes nothing.
std::string collapseHome(const std::string &path, const std::string &home);

// collapseHome with the running user's home folder (HOME, or USERPROFILE on Windows).
std::string collapseHome(const std::string &path);

} // namespace sfcop::app
