// The game's path model (Kernel.h): Win32 names to game paths, the per-user copy of each, the
// current directory and the calls that only answer names, drives and volumes.

#include "hle/kernel/Kernel.h"

#include <algorithm>

namespace sfcop::hle::kernel {
namespace {

using platform::Error;
using platform::Root;

constexpr std::int64_t kFileTimeEpoch = 116444736000000000LL; // 1970-01-01 as a FILETIME

char upper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }
char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
bool isLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

std::string lowered(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), lower);
    return out;
}

bool startsWithNoCase(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && lowered(text.substr(0, prefix.size())) == lowered(prefix);
}

// The characters Windows refuses in a path (wildcards included: a search splits them off first).
bool refusedCharacter(char c) {
    return static_cast<unsigned char>(c) < 0x20 || c == '<' || c == '>' || c == '"' || c == '|' ||
           c == '?' || c == '*';
}

char driveOf(std::string_view absolute) { return absolute.empty() ? '\0' : absolute[0]; }

std::string_view firstSegment(std::string_view relative) {
    return relative.substr(0, relative.find('/'));
}

} // namespace

bool isUserFolder(std::string_view segment) {
    std::string name = lowered(segment);
    return name == "saves" || name == "replays" || name == "mods" || name == "settings" ||
           name == "logs";
}

std::uint64_t fileTimeFromNs(std::int64_t ns) {
    return static_cast<std::uint64_t>(ns / 100 + kFileTimeEpoch);
}

std::int64_t nsFromFileTime(std::uint64_t fileTime) {
    return (static_cast<std::int64_t>(fileTime) - kFileTimeEpoch) * 100;
}

std::uint32_t win32Error(Error error, bool writing) {
    switch (error) {
    case Error::kNone:
        return 0;
    case Error::kNotFound:
        return win32::kErrorFileNotFound;
    case Error::kAccessDenied:
    case Error::kWrongState:
        return win32::kErrorAccessDenied;
    case Error::kAlreadyExists:
        return win32::kErrorFileExists;
    case Error::kInvalidArgument:
        return win32::kErrorInvalidName;
    case Error::kInvalidHandle:
        return win32::kErrorInvalidHandle;
    case Error::kOutOfResources:
        return win32::kErrorTooManyOpenFiles;
    case Error::kIoError:
        return writing ? win32::kErrorWriteFault : win32::kErrorReadFault;
    case Error::kUnsupported:
        return win32::kErrorNotSupported;
    default:
        return win32::kErrorGenFailure;
    }
}

Kernel::Kernel(platform::Store &store, KernelConfig config)
    : store_(store), config_(std::move(config)) {
    if (config_.gameFolder.empty() || config_.gameFolder.back() != '\\') {
        config_.gameFolder += '\\';
    }
    config_.gameFolder[0] = upper(config_.gameFolder[0]);
    currentDirectory_ = config_.gameFolder.size() > 3
                            ? config_.gameFolder.substr(0, config_.gameFolder.size() - 1)
                            : config_.gameFolder;
}

std::optional<std::uint32_t> Kernel::TakeLastError() {
    std::optional<std::uint32_t> error = lastError_;
    lastError_.reset();
    return error;
}

std::string Kernel::Absolute(std::string_view name) const {
    std::string path(name);
    std::replace(path.begin(), path.end(), '/', '\\');
    if (path.empty() || path.compare(0, 2, "\\\\") == 0) {
        return {};
    }
    for (std::size_t i = 0; i < path.size(); i++) {
        if (refusedCharacter(path[i]) || (path[i] == ':' && (i != 1 || !isLetter(path[0])))) {
            return {};
        }
    }
    const std::string &cwd = currentDirectory_;
    std::string joined;
    if (path.size() >= 2 && path[1] == ':') {
        char drive = upper(path[0]);
        std::string rest = path.substr(2);
        if (!rest.empty() && rest[0] == '\\') {
            joined = std::string(1, drive) + ":" + rest;
        } else if (drive == driveOf(cwd)) {
            joined = cwd + "\\" + rest;
        } else {
            joined = std::string(1, drive) + ":\\" + rest;
        }
    } else if (path[0] == '\\') {
        joined = cwd.substr(0, 2) + path;
    } else {
        joined = cwd + "\\" + path;
    }

    std::vector<std::string> segments;
    std::size_t start = 3; // after "X:\"
    while (start <= joined.size()) {
        std::size_t end = joined.find('\\', start);
        if (end == std::string::npos) {
            end = joined.size();
        }
        std::string segment = joined.substr(start, end - start);
        start = end + 1;
        if (segment == "..") {
            if (!segments.empty()) {
                segments.pop_back();
            }
            continue;
        }
        while (!segment.empty() && (segment.back() == '.' || segment.back() == ' ')) {
            segment.pop_back();
        }
        if (!segment.empty()) {
            segments.push_back(segment);
        }
    }
    std::string out = joined.substr(0, 2) + "\\";
    for (std::size_t i = 0; i < segments.size(); i++) {
        out += (i == 0 ? "" : "\\") + segments[i];
    }
    return out;
}

GamePath Kernel::Locate(std::string_view name) const {
    GamePath where;
    std::string absolute = Absolute(name);
    if (absolute.empty()) {
        return where;
    }
    std::string withSlash = absolute.back() == '\\' ? absolute : absolute + "\\";
    if (!startsWithNoCase(withSlash, config_.gameFolder)) {
        return where;
    }
    where.inGame = true;
    where.relative = withSlash.substr(config_.gameFolder.size());
    if (!where.relative.empty()) {
        where.relative.pop_back();
    }
    std::replace(where.relative.begin(), where.relative.end(), '\\', '/');
    return where;
}

std::string Kernel::UserPathOf(std::string_view relative) {
    if (isUserFolder(firstSegment(relative))) {
        std::string first = lowered(firstSegment(relative));
        return first + std::string(relative.substr(first.size()));
    }
    std::string_view last = relative.substr(relative.rfind('/') + 1);
    std::size_t dot = last.rfind('.');
    std::string extension = dot == std::string_view::npos ? "" : lowered(last.substr(dot + 1));
    const char *folder = extension == "txt" || extension == "log" ? "logs" : "settings";
    return relative.empty() ? std::string(folder) : std::string(folder) + "/" + std::string(relative);
}

std::optional<Kernel::Entry> Kernel::Lookup(std::string_view relative) {
    if (relative.empty()) {
        Entry folder;
        folder.info.isDirectory = true;
        return folder;
    }
    std::string user = UserPathOf(relative);
    platform::Result<platform::FileInfo> mine = store_.Stat(Root::kUser, user);
    if (mine) {
        return Entry{Root::kUser, user, mine.Value()};
    }
    if (isUserFolder(firstSegment(relative))) {
        return std::nullopt; // per-user only: the install ships none of these folders
    }
    platform::Result<platform::FileInfo> installed = store_.Stat(Root::kInstall, relative);
    if (installed) {
        Entry entry{Root::kInstall, std::string(relative), installed.Value()};
        entry.info.readOnly = false; // the game may write it: the write goes to its copy
        return entry;
    }
    // A folder that so far holds only per-user logs.
    std::string logs = "logs/" + std::string(relative);
    if (logs != user) {
        platform::Result<platform::FileInfo> logged = store_.Stat(Root::kUser, logs);
        if (logged && logged.Value().isDirectory) {
            return Entry{Root::kUser, logs, logged.Value()};
        }
    }
    return std::nullopt;
}

bool Kernel::IsDirectory(std::string_view relative) {
    std::optional<Entry> entry = Lookup(relative);
    return entry && entry->info.isDirectory;
}

std::uint32_t Kernel::MissingError(std::string_view relative) {
    std::size_t slash = relative.rfind('/');
    std::string_view parent = slash == std::string_view::npos ? "" : relative.substr(0, slash);
    return IsDirectory(parent) ? win32::kErrorFileNotFound : win32::kErrorPathNotFound;
}

platform::Status Kernel::MakeUserParent(std::string_view userPath) {
    std::size_t slash = userPath.rfind('/');
    if (slash == std::string_view::npos) {
        return {};
    }
    return store_.MakeDirectory(Root::kUser, userPath.substr(0, slash));
}

bool Kernel::SetCurrentDirectory(std::string_view name) {
    GamePath where = Locate(name);
    if (!where.inGame) {
        SetError(win32::kErrorPathNotFound);
        return false;
    }
    std::optional<Entry> entry = Lookup(where.relative);
    if (!entry) {
        SetError(MissingError(where.relative));
        return false;
    }
    if (!entry->info.isDirectory) {
        SetError(win32::kErrorDirectory);
        return false;
    }
    currentDirectory_ = Absolute(name);
    return true;
}

std::optional<std::string> Kernel::FullPathName(std::string_view name, std::size_t *filePart) {
    std::string absolute = Absolute(name);
    if (absolute.empty()) {
        SetError(win32::kErrorInvalidName);
        return std::nullopt;
    }
    char last = name.back();
    if ((last == '\\' || last == '/') && absolute.back() != '\\') {
        absolute += '\\';
    }
    if (filePart != nullptr) {
        *filePart = absolute.back() == '\\' ? std::string::npos : absolute.rfind('\\') + 1;
    }
    return absolute;
}

std::optional<std::string> Kernel::ShortPathName(std::string_view name) {
    GamePath where = Locate(name);
    if (!where.inGame) {
        SetError(win32::kErrorPathNotFound);
        return std::nullopt;
    }
    if (!Lookup(where.relative)) {
        SetError(MissingError(where.relative));
        return std::nullopt;
    }
    return std::string(name); // no 8.3 names: the long name is the short one
}

std::uint32_t Kernel::DriveType(std::string_view root) {
    char drive = root.empty() ? driveOf(currentDirectory_) : upper(root[0]);
    if (!root.empty() && (!isLetter(root[0]) || (root.size() > 1 && root[1] != ':'))) {
        return win32::kDriveNoRootDir;
    }
    return drive == driveOf(config_.gameFolder) ? win32::kDriveFixed : win32::kDriveNoRootDir;
}

bool Kernel::DiskFreeSpace(std::string_view root, std::uint32_t *sectorsPerCluster,
                           std::uint32_t *bytesPerSector, std::uint32_t *freeClusters,
                           std::uint32_t *totalClusters) {
    if (DriveType(root) != win32::kDriveFixed) {
        SetError(win32::kErrorPathNotFound);
        return false;
    }
    // 2 GB free of 4 GB: a game of this age multiplies these in 32 bits.
    if (sectorsPerCluster != nullptr) {
        *sectorsPerCluster = 8;
    }
    if (bytesPerSector != nullptr) {
        *bytesPerSector = 512;
    }
    if (freeClusters != nullptr) {
        *freeClusters = 0x7FFFF;
    }
    if (totalClusters != nullptr) {
        *totalClusters = 0xFFFFF;
    }
    return true;
}

std::optional<Kernel::VolumeInfo> Kernel::VolumeInformation(std::string_view root) {
    if (DriveType(root) != win32::kDriveFixed) {
        SetError(win32::kErrorPathNotFound);
        return std::nullopt;
    }
    VolumeInfo volume;
    volume.name = "SFCOP";
    volume.serial = 0x5FC00255u;
    volume.maxComponentLength = 255;
    volume.flags = 0x00000006u; // FS_CASE_IS_PRESERVED | FS_UNICODE_STORED_ON_DISK
    volume.fileSystem = "NTFS";
    return volume;
}

} // namespace sfcop::hle::kernel
