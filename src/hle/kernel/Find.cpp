// FindFirstFile and FindNextFile: a game directory as the install and the per-user copies
// together, filtered by a Windows wildcard and handed out in NTFS's order.

#include "hle/kernel/Kernel.h"

#include <algorithm>

namespace sfcop::hle::kernel {
namespace {

using platform::Root;

char upper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }

std::string uppered(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), upper);
    return out;
}

bool globMatch(std::string_view mask, std::string_view name) {
    while (!mask.empty()) {
        if (mask.front() == '*') {
            mask.remove_prefix(1);
            if (mask.empty()) {
                return true;
            }
            for (std::size_t i = 0; i <= name.size(); i++) {
                if (globMatch(mask, name.substr(i))) {
                    return true;
                }
            }
            return false;
        }
        if (name.empty() || (mask.front() != '?' && upper(mask.front()) != upper(name.front()))) {
            return false;
        }
        mask.remove_prefix(1);
        name.remove_prefix(1);
    }
    return name.empty();
}

// The wildcard rules a Windows search applies to long names: `*` any run, `?` one character,
// `*.*` everything, a mask ending in `.*` also a name without a dot, and one ending in a single
// `.` a name without an extension. Without case.
bool wildcardMatch(std::string_view mask, std::string_view name) {
    if (mask == "*" || mask == "*.*") {
        return true;
    }
    if (globMatch(mask, name)) {
        return true;
    }
    if (mask.size() > 2 && mask.substr(mask.size() - 2) == ".*" &&
        name.find('.') == std::string_view::npos) {
        return globMatch(mask.substr(0, mask.size() - 2), name);
    }
    if (mask.size() > 1 && mask.back() == '.' && mask[mask.size() - 2] != '.' &&
        name.find('.') == std::string_view::npos) {
        return globMatch(mask.substr(0, mask.size() - 1), name);
    }
    return false;
}

std::uint32_t attributesOf(const platform::FileInfo &info) {
    if (info.isDirectory) {
        return win32::kAttributeDirectory;
    }
    return win32::kAttributeArchive | (info.readOnly ? win32::kAttributeReadOnly : 0u);
}

FindData findDataOf(const std::string &name, const platform::FileInfo &info) {
    FindData data;
    data.attributes = attributesOf(info);
    std::uint64_t time = fileTimeFromNs(info.modifiedNs);
    data.creationTime = time;
    data.accessTime = time;
    data.writeTime = time;
    data.size = info.isDirectory ? 0 : info.size;
    data.name = name;
    return data;
}

// The name of a file Store's atomic write is still writing: `.<name>.tmp<digits>`.
bool isStoreTemporary(const std::string &name) {
    if (name.size() < 2 || name[0] != '.') {
        return false;
    }
    std::size_t at = name.rfind(".tmp");
    if (at == std::string::npos || at == 0 || at + 4 == name.size()) {
        return false;
    }
    return name.find_first_not_of("0123456789", at + 4) == std::string::npos;
}

std::string lowered(std::string_view text) {
    std::string out(text);
    for (char &c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

} // namespace

std::map<std::string, FindData> Kernel::ListDirectory(std::string_view relative) {
    std::map<std::string, FindData> merged; // by lowered name
    auto add = [&](Root root, const std::string &path, bool directoriesOnly) {
        platform::Result<std::vector<platform::DirectoryEntry>> listed = store_.List(root, path);
        if (!listed) {
            return;
        }
        for (const platform::DirectoryEntry &entry : listed.Value()) {
            if (directoriesOnly && !entry.info.isDirectory) {
                continue;
            }
            if (root == Root::kUser && isStoreTemporary(entry.name)) {
                continue; // an atomic write in progress
            }
            platform::FileInfo info = entry.info;
            if (root == Root::kInstall) {
                info.readOnly = false;
            }
            merged[lowered(entry.name)] = findDataOf(entry.name, info);
        }
    };
    if (isUserFolder(relative.substr(0, relative.find('/')))) {
        add(Root::kUser, UserPathOf(relative), false);
        return merged;
    }
    std::string rel(relative);
    add(Root::kInstall, rel, false);
    add(Root::kUser, rel.empty() ? "logs" : "logs/" + rel, false);
    add(Root::kUser, rel.empty() ? "settings" : "settings/" + rel, false);
    if (rel.empty()) {
        for (const char *folder : {"saves", "replays", "mods"}) {
            platform::Result<platform::FileInfo> info = store_.Stat(Root::kUser, folder);
            if (info && info.Value().isDirectory) {
                merged[folder] = findDataOf(folder, info.Value());
            }
        }
    }
    return merged;
}

std::uint32_t Kernel::FindFirstFile(std::string_view pattern, FindData *first) {
    std::string text(pattern);
    std::replace(text.begin(), text.end(), '/', '\\');
    std::size_t split = text.rfind('\\');
    if (split == std::string::npos && text.size() >= 2 && text[1] == ':') {
        split = 1;
    }
    std::string folder = split == std::string::npos ? "." : text.substr(0, split + 1);
    std::string mask = split == std::string::npos ? text : text.substr(split + 1);
    if (mask.empty()) {
        SetError(win32::kErrorFileNotFound);
        return 0;
    }
    if (mask.find_first_of("<>\"|:\\") != std::string::npos) {
        SetError(win32::kErrorInvalidName);
        return 0;
    }
    if (split == 1 && text[1] == ':') {
        folder += '.'; // "C:*.txt": the current directory of that drive
    }
    GamePath where = Locate(folder);
    if (!where.inGame || !IsDirectory(where.relative)) {
        SetError(win32::kErrorPathNotFound);
        return 0;
    }

    Search search;
    if (Absolute(folder).size() > 3) { // not a drive's root: it has the dot entries
        platform::FileInfo dir;
        dir.isDirectory = true;
        for (const char *dot : {".", ".."}) {
            if (wildcardMatch(mask, dot)) {
                search.entries.push_back(findDataOf(dot, dir));
            }
        }
    }
    std::vector<FindData> found;
    for (auto &entry : ListDirectory(where.relative)) {
        if (wildcardMatch(mask, entry.second.name)) {
            found.push_back(entry.second);
        }
    }
    std::sort(found.begin(), found.end(), [](const FindData &a, const FindData &b) {
        return uppered(a.name) < uppered(b.name);
    });
    search.entries.insert(search.entries.end(), found.begin(), found.end());
    if (search.entries.empty()) {
        SetError(win32::kErrorFileNotFound);
        return 0;
    }
    if (first != nullptr) {
        *first = search.entries[0];
    }
    search.next = 1;
    std::uint32_t id = nextId_++;
    finds_[id] = std::move(search);
    return id;
}

bool Kernel::FindNextFile(std::uint32_t find, FindData *next) {
    auto search = finds_.find(find);
    if (search == finds_.end()) {
        SetError(win32::kErrorInvalidHandle);
        return false;
    }
    if (search->second.next >= search->second.entries.size()) {
        SetError(win32::kErrorNoMoreFiles);
        return false;
    }
    if (next != nullptr) {
        *next = search->second.entries[search->second.next];
    }
    search->second.next++;
    return true;
}

bool Kernel::FindClose(std::uint32_t find) {
    if (finds_.erase(find) == 0) {
        SetError(win32::kErrorInvalidHandle);
        return false;
    }
    return true;
}

} // namespace sfcop::hle::kernel
