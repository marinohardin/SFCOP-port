// TEMPORARY: the few file operations the application skeleton needs, until the Store component
// (src/platform/Store/Store.h) lands; the integrator replaces this interface with Store's at gate
// G0. Paths are UTF-8 strings; '/' separates components on every platform, and Windows accepts
// '\' as well.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace sfcop::app {

// A file opened for reading, front to back.
class FileReader {
public:
    virtual ~FileReader() = default;
    // Reads up to `size` bytes; returns how many, 0 at the end or after an error.
    virtual std::size_t read(unsigned char *buffer, std::size_t size) = 0;
    // True once a read has failed for a reason other than the end of the file.
    virtual bool failed() const = 0;
};

class FileSystem {
public:
    virtual ~FileSystem() = default;
    virtual bool isDirectory(const std::string &path) const = 0;
    // The size of a regular file; nothing when there is no regular file at `path`.
    virtual std::optional<std::uint64_t> fileSize(const std::string &path) const = 0;
    // Nothing when the file cannot be opened.
    virtual std::unique_ptr<FileReader> openRead(const std::string &path) const = 0;
    // Creates the folder and its parents; true when the folder exists afterwards.
    virtual bool createDirectories(const std::string &path) = 0;
    // A whole small file (a remembered path, a hash list); nothing when it cannot be read.
    virtual std::optional<std::string> readText(const std::string &path) const = 0;
    // Writes to a temporary file beside `path` and renames it over `path` (DESIGN §8).
    virtual bool writeTextAtomic(const std::string &path, const std::string &text) = 0;
    // `path` made absolute against the current folder, '.' and '..' resolved; unchanged when it
    // is absolute already.
    virtual std::string absolutePath(const std::string &path) const = 0;
};

// The real file system.
FileSystem &hostFileSystem();

// `dir` and `relative` ('/' separated) joined without doubling a separator `dir` ends with. A
// `dir` written with '\' only (a Windows path from the environment) keeps '\' throughout.
std::string joinPath(const std::string &dir, const std::string &relative);

} // namespace sfcop::app
