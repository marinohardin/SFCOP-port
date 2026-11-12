// The file operations an import needs (Import.h), beyond the few of Files.h: listing a folder,
// writing and cloning files, renaming and removing folders, and the free space on a volume.
// Paths are UTF-8 strings with '/' between components, as in Files.h. One interface so that the
// tests can make a volume full, a write fail or a clone unavailable.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/Files.h"

namespace sfcop::app {

// A file opened for writing, created or emptied first.
class FileWriter {
public:
    virtual ~FileWriter() = default;
    // Writes all `size` bytes; false on any error.
    virtual bool write(const unsigned char *buffer, std::size_t size) = 0;
    // Flushes and closes; false when anything written was lost.
    virtual bool close() = 0;
};

enum class EntryKind { kFile, kFolder, kOther };

struct FolderEntry {
    std::string name;
    // A symbolic link is kOther, whatever it points at: a search never follows one (a Wine
    // prefix links the whole disk under dosdevices/).
    EntryKind kind = EntryKind::kOther;
};

class ImportFiles {
public:
    virtual ~ImportFiles() = default;
    // Nothing when nothing is at `path`; a symbolic link is kOther.
    virtual std::optional<EntryKind> kindOf(const std::string &path) const = 0;
    // As kindOf, but a symbolic link is what it points at; nothing when it points nowhere.
    virtual std::optional<EntryKind> kindFollowing(const std::string &path) const = 0;
    virtual std::optional<std::uint64_t> fileSize(const std::string &path) const = 0;
    // The folder's entries sorted by name (byte order); empty when it cannot be read.
    virtual std::vector<FolderEntry> list(const std::string &dir) const = 0;
    virtual std::unique_ptr<FileReader> openRead(const std::string &path) const = 0;
    virtual std::unique_ptr<FileWriter> openWrite(const std::string &path) = 0;
    // A copy-on-write clone of `from` at `to` (APFS clonefile, a reflink on Linux); false when the
    // file system cannot clone between the two, and the caller copies instead.
    virtual bool clone(const std::string &from, const std::string &to) = 0;
    virtual bool createDirectories(const std::string &path) = 0;
    // Renames a file or folder; `to` must not exist.
    virtual bool rename(const std::string &from, const std::string &to) = 0;
    // Removes a file or a folder and everything in it; true when nothing is left at `path`.
    virtual bool removeAll(const std::string &path) = 0;
    // The bytes free for this user on the volume `path` is on, or would be on (the nearest folder
    // above it that exists); nothing when it cannot be found.
    virtual std::optional<std::uint64_t> freeBytes(const std::string &path) const = 0;
    // `path` absolute, with every link in the part that exists resolved and "." and ".." gone;
    // the part that does not exist yet is kept as given. A Windows short (8.3) name is not
    // expanded, and only ASCII letters are compared without case, so a path spelt either way
    // there is not recognised as the same folder.
    virtual std::string resolved(const std::string &path) const = 0;
};

// The real file system.
ImportFiles &hostImportFiles();

// The platform's copy-on-write clone (ImportClone_<platform>.cpp); false when there is none.
bool cloneHostFile(const std::string &from, const std::string &to);

} // namespace sfcop::app
