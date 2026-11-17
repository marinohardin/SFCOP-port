#include "app/FileMenu.h"

namespace sfcop::app {

std::vector<FileMenuEntry> fileMenuEntries(const std::string &dataDir, const std::string &logsDir) {
    std::vector<FileMenuEntry> entries;
    if (!dataDir.empty()) {
        entries.push_back({"Show Game Files in Finder", dataDir});
    }
    if (!logsDir.empty()) {
        entries.push_back({"Show Logs in Finder", logsDir});
    }
    return entries;
}

bool activateFileMenuEntry(const std::vector<FileMenuEntry> &entries, std::size_t index,
                           const RevealFn &reveal) {
    if (index >= entries.size() || !reveal) {
        return false;
    }
    return reveal(entries[index].path);
}

} // namespace sfcop::app
