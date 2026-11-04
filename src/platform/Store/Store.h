// Store: every file the game reads or writes and its registry values (DESIGN §4.7, §6.2, §6.6,
// §8). Two roots: the install, read-only, and the per-user data directory, which holds
// settings/, saves/, replays/, logs/, mods/ and registry.tsv. Store is the only code that knows
// where they are (C4 decides the per-user location; Store exposes it). The kernel and advapi32
// HLE modules decide which root a game path goes to; Store only resolves paths inside a root.
//
// Paths are relative to a root, UTF-8, with `/` between segments; no empty, `.` or `..` segment,
// no leading `/`, no drive letter, no `\`. Lookup ignores ASCII case (the game's paths come in
// any case and Linux file systems are case-sensitive); a created file keeps the case it was given.
//
// Threading: every method may be called from any thread. One FileHandle is used by one thread at
// a time; different handles may be used at once. Registry calls are atomic with each other.
#pragma once

#include "platform/Core/Handle.h"
#include "platform/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sfcop::platform {

struct FileTag {};
using FileHandle = Handle<FileTag>;

enum class Root : std::uint8_t {
    kInstall, // the player's install, read-only
    kUser,    // the per-user data directory
};

// The per-user subdirectories of DESIGN §6.6.
enum class UserFolder : std::uint8_t { kSettings, kSaves, kReplays, kLogs, kMods };

// The folder's name under the per-user root, for building paths: `settings`, `saves`, ...
constexpr const char *userFolderName(UserFolder folder) {
    switch (folder) {
    case UserFolder::kSettings:
        return "settings";
    case UserFolder::kSaves:
        return "saves";
    case UserFolder::kReplays:
        return "replays";
    case UserFolder::kLogs:
        return "logs";
    case UserFolder::kMods:
        return "mods";
    }
    return "";
}

enum class OpenMode : std::uint8_t {
    kRead,         // existing file, read only
    kReadWrite,    // existing file, read and write
    kCreate,       // create or truncate to empty, read and write
    kCreateNew,    // create; kAlreadyExists if it exists
    kOpenOrCreate, // open, or create empty if missing; read and write; content kept
};

enum class SeekOrigin : std::uint8_t { kBegin, kCurrent, kEnd };

struct FileInfo {
    std::uint64_t size = 0; // bytes; 0 for a directory
    bool isDirectory = false;
    bool readOnly = false;          // always true under kInstall
    std::int64_t modifiedNs = 0;    // nanoseconds since 1970-01-01 00:00:00 UTC
};

struct DirectoryEntry {
    std::string name; // one segment, as stored on disk
    FileInfo info;
};

// The registry value types the game uses (REG_DWORD, REG_SZ, REG_BINARY).
enum class RegistryType : std::uint8_t { kDword, kString, kBinary };

struct RegistryValue {
    RegistryType type = RegistryType::kDword;
    std::uint32_t dword = 0;          // kDword
    std::string text;                 // kString: the bytes the game stored, without the NUL
    std::vector<std::uint8_t> bytes;  // kBinary
};

class Store {
public:
    virtual ~Store() = default;

    // The absolute host path of a root, UTF-8, for diagnostics and the HLE module's answers to
    // the game's directory queries. Errors: kUnsupported; kNotFound if the root is not set up
    // (the install was not found, or the per-user directory could not be created).
    virtual Result<std::string> RootPath(Root root) = 0;

    // Opens a file. Under kInstall only kRead is allowed. Errors: kUnsupported;
    // kInvalidArgument for a malformed path; kAccessDenied for a write mode under kInstall or a
    // read-only file; kNotFound (kRead, kReadWrite) if the file or a parent directory is
    // missing; kAlreadyExists (kCreateNew); kWrongState if the path is a directory;
    // kOutOfResources; kIoError.
    virtual Result<FileHandle> Open(Root root, std::string_view path, OpenMode mode) = 0;

    // Reads up to `bytes` from the current position into `out` and advances it; returns how many
    // were read, 0 at end of file. Errors: kUnsupported; kInvalidHandle; kIoError.
    virtual Result<std::size_t> Read(FileHandle file, void *out, std::size_t bytes) = 0;

    // Writes `bytes` from `data` at the current position, extending the file if needed, and
    // advances it; returns how many were written (all of them unless an error is returned).
    // Errors: kUnsupported; kInvalidHandle; kAccessDenied if opened kRead; kIoError (disk full).
    virtual Result<std::size_t> Write(FileHandle file, const void *data, std::size_t bytes) = 0;

    // Moves the position and returns the new one from the start. A position past the end is
    // allowed; a write there fills the gap with zeros. Errors: kUnsupported; kInvalidHandle;
    // kInvalidArgument if the result would be negative.
    virtual Result<std::uint64_t> Seek(FileHandle file, std::int64_t offset,
                                       SeekOrigin origin) = 0;

    // Size, type and time of an open file. Errors: kUnsupported; kInvalidHandle; kIoError.
    virtual Result<FileInfo> GetInfo(FileHandle file) = 0;

    // Truncates or zero-extends the file to `bytes`; the position is unchanged. Errors:
    // kUnsupported; kInvalidHandle; kAccessDenied if opened kRead; kIoError.
    virtual Status SetSize(FileHandle file, std::uint64_t bytes) = 0;

    // Sets the file's modification time. Errors: kUnsupported; kInvalidHandle; kAccessDenied if
    // opened kRead; kIoError.
    virtual Status SetModifiedTime(FileHandle file, std::int64_t modifiedNs) = 0;

    // Writes buffered data through to the operating system. Errors: kUnsupported;
    // kInvalidHandle; kIoError.
    virtual Status Flush(FileHandle file) = 0;

    // Flushes and closes. The handle is dead afterwards even if an error is returned. Errors:
    // kUnsupported; kInvalidHandle; kIoError (a final write failed).
    virtual Status Close(FileHandle file) = 0;

    // Size, type and time by path, without opening. Errors: kUnsupported; kInvalidArgument;
    // kNotFound; kIoError.
    virtual Result<FileInfo> Stat(Root root, std::string_view path) = 0;

    // The entries of a directory (not `.` or `..`), sorted by name in byte order so that the
    // order is the same on every platform. An empty path lists the root. Errors: kUnsupported;
    // kInvalidArgument; kNotFound; kWrongState if the path is a file; kIoError.
    virtual Result<std::vector<DirectoryEntry>> List(Root root, std::string_view path) = 0;

    // Creates a directory under kUser and any missing parents; an existing directory succeeds.
    // Errors: kUnsupported; kInvalidArgument; kAccessDenied under kInstall; kAlreadyExists if a
    // file has that path; kIoError.
    virtual Status MakeDirectory(Root root, std::string_view path) = 0;

    // Deletes a file or an empty directory under kUser. Errors: kUnsupported; kInvalidArgument;
    // kAccessDenied under kInstall or if the file is open; kNotFound; kWrongState if the
    // directory is not empty; kIoError.
    virtual Status Remove(Root root, std::string_view path) = 0;

    // Renames a file or directory under kUser, replacing an existing file at `to`. Errors:
    // kUnsupported; kInvalidArgument; kAccessDenied under kInstall; kNotFound if `from` is
    // missing or `to`'s parent is; kAlreadyExists if `to` is a directory; kIoError.
    virtual Status Rename(Root root, std::string_view from, std::string_view to) = 0;

    // Replaces the file at `path` under kUser with `bytes` from `data` so that a reader, or a
    // crash, sees either the old content or the new, never a mix (write a temporary file in the
    // same directory, flush, rename over). The parent directory must exist. Errors:
    // kUnsupported; kInvalidArgument; kAccessDenied under kInstall; kNotFound if the parent is
    // missing; kIoError (the old file is then unchanged).
    virtual Status WriteAtomic(Root root, std::string_view path, const void *data,
                               std::size_t bytes) = 0;

    // Registry. `key` is a `\`-separated path whose first segment is the root key's name as the
    // HLE module spells it (e.g. `HKEY_LOCAL_MACHINE\Software\TestCompany`); keys and value names
    // compare without regard to ASCII case. The empty value name is the key's default value.
    // Every successful change is written to registry.tsv with WriteAtomic() before the call
    // returns.

    // Whether `key` exists. Errors: kUnsupported; kInvalidArgument for an empty key or segment.
    virtual Result<bool> KeyExists(std::string_view key) = 0;

    // Creates `key` and any missing parents; an existing key succeeds. Errors: kUnsupported;
    // kInvalidArgument; kIoError if registry.tsv cannot be written (nothing changes).
    virtual Status CreateKey(std::string_view key) = 0;

    // Deletes `key`, its values and every key below it. Errors: kUnsupported;
    // kInvalidArgument (also for a root key alone); kNotFound; kIoError (nothing changes).
    virtual Status DeleteKey(std::string_view key) = 0;

    // A value of an existing key. Errors: kUnsupported; kInvalidArgument; kNotFound if the key or
    // the value is missing.
    virtual Result<RegistryValue> GetValue(std::string_view key, std::string_view name) = 0;

    // Sets a value, replacing any of the same name whatever its type. The key must exist.
    // Errors: kUnsupported; kInvalidArgument (also for a text or name containing a tab or a line
    // break, which registry.tsv cannot hold); kNotFound if the key is missing; kIoError (nothing
    // changes).
    virtual Status SetValue(std::string_view key, std::string_view name,
                            const RegistryValue &value) = 0;
};

// A Store whose every method fails with kUnsupported.
std::unique_ptr<Store> makeStoreStub();

// The build's real backend, on the host file system (B5); declared here so that the client needs
// no backend header, defined by the backend's own source file once it lands. The two roots are
// absolute host paths in UTF-8: the verified install (read-only) and the per-user data folder
// (`Startup::installDir` and `Startup::dataDir` in src/app/Startup.h); the backend creates
// nothing under the install and may create registry.tsv under the per-user root.
std::unique_ptr<Store> makeStore(std::string installDir, std::string userDir);

} // namespace sfcop::platform
