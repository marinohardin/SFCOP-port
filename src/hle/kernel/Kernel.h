// KERNEL32's files, directories, module path and .ini profiles, on Store (DESIGN §5.3, §6.1,
// §6.6). Plain C++: KernelGuest.c reads the game's arguments and calls this through
// KernelBridge.h.
//
// The game's path model. The game sees one folder, the game folder (`C:\` with the exe in it, as
// it was installed); its Win32 paths (a drive letter, `\` or `/`, relative to the current
// directory) are resolved to a path under that folder. What it sees there is the install with the
// per-user folder laid over it:
//   - a read finds the per-user copy of a file first, then the install's;
//   - every write goes to the per-user copy (a file opened to change in place is copied there
//     first), so the install is only ever read;
//   - where the per-user copy of a game path is: under `saves/`, `replays/`, `mods/`, `settings/`
//     or `logs/` as it is when the path starts with that folder; else under `logs/` for a `.txt`
//     or `.log` file and under `settings/` for anything else, at the same path
//     (`ClientSettings.gf` is `settings/ClientSettings.gf`, `Assets/x.txt` is
//     `logs/Assets/x.txt`, the game's own `settings\x` is `settings/x`).
//   - those five folders are per-user only: the install ships none of them, so a lookup under
//     one never asks the install.
// A file only in the install cannot be deleted or renamed (ERROR_ACCESS_DENIED); deleting a
// per-user copy shows the install's again.
//
// Strings are UTF-8 here; KernelBridge converts the game's ANSI and UTF-16. Each call sets the
// Win32 last error where the original sets one; TakeLastError() hands it to the bridge, which
// moves it, under its lock and with the call, to the calling thread's own last error for the
// shim to store where GetLastError reads it (KernelBridge.cpp). A Kernel used directly keeps one.
#pragma once

#include "platform/Store/Store.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sfcop::hle::kernel {

// The Win32 values these calls take and answer.
namespace win32 {
constexpr std::uint32_t kErrorFileNotFound = 2;
constexpr std::uint32_t kErrorPathNotFound = 3;
constexpr std::uint32_t kErrorTooManyOpenFiles = 4;
constexpr std::uint32_t kErrorAccessDenied = 5;
constexpr std::uint32_t kErrorInvalidHandle = 6;
constexpr std::uint32_t kErrorNotEnoughMemory = 8;
constexpr std::uint32_t kErrorNoMoreFiles = 18;
constexpr std::uint32_t kErrorWriteFault = 29;
constexpr std::uint32_t kErrorReadFault = 30;
constexpr std::uint32_t kErrorGenFailure = 31;
constexpr std::uint32_t kErrorNotSupported = 50;
constexpr std::uint32_t kErrorFileExists = 80;
constexpr std::uint32_t kErrorInvalidParameter = 87;
constexpr std::uint32_t kErrorInvalidName = 123;
constexpr std::uint32_t kErrorNegativeSeek = 131;
constexpr std::uint32_t kErrorDirNotEmpty = 145;
constexpr std::uint32_t kErrorAlreadyExists = 183;
constexpr std::uint32_t kErrorDirectory = 267;

constexpr std::uint32_t kGenericRead = 0x80000000u;
constexpr std::uint32_t kGenericWrite = 0x40000000u;
constexpr std::uint32_t kGenericAll = 0x10000000u;
constexpr std::uint32_t kFileReadData = 0x0001u;
constexpr std::uint32_t kFileWriteData = 0x0002u;
constexpr std::uint32_t kFileAppendData = 0x0004u;

constexpr std::uint32_t kCreateNew = 1;
constexpr std::uint32_t kCreateAlways = 2;
constexpr std::uint32_t kOpenExisting = 3;
constexpr std::uint32_t kOpenAlways = 4;
constexpr std::uint32_t kTruncateExisting = 5;

constexpr std::uint32_t kFileBegin = 0;
constexpr std::uint32_t kFileCurrent = 1;
constexpr std::uint32_t kFileEnd = 2;
constexpr std::uint32_t kInvalidSetFilePointer = 0xFFFFFFFFu;
constexpr std::uint32_t kInvalidFileSize = 0xFFFFFFFFu;
constexpr std::uint32_t kInvalidFileAttributes = 0xFFFFFFFFu;

constexpr std::uint32_t kAttributeReadOnly = 0x01u;
constexpr std::uint32_t kAttributeDirectory = 0x10u;
constexpr std::uint32_t kAttributeArchive = 0x20u;

constexpr std::uint32_t kDriveNoRootDir = 1;
constexpr std::uint32_t kDriveFixed = 3;
} // namespace win32

// Whether a first path segment (any case) is one of the five per-user folders: `saves`,
// `replays`, `mods`, `settings` and `logs`.
bool isUserFolder(std::string_view segment);

// FILETIME: 100 ns intervals since 1601-01-01 UTC.
std::uint64_t fileTimeFromNs(std::int64_t ns);
std::int64_t nsFromFileTime(std::uint64_t fileTime);

struct KernelConfig {
    std::string gameFolder = "C:\\"; // the game folder as the game sees it, ending in `\`
    std::string exeName = "StarFleetOP.exe";
    std::string windowsDirectory = "C:\\WINDOWS";
    std::string systemDirectory = "C:\\WINDOWS\\SYSTEM";
};

// One entry of a directory search, as WIN32_FIND_DATA carries it.
struct FindData {
    std::uint32_t attributes = 0;
    std::uint64_t creationTime = 0; // FILETIMEs; all three are the modification time
    std::uint64_t accessTime = 0;
    std::uint64_t writeTime = 0;
    std::uint64_t size = 0;
    std::string name; // UTF-8
};

// Where a game path lies: its path under the game folder, '/'-separated, in the case given.
struct GamePath {
    bool inGame = false;
    std::string relative; // empty: the game folder itself
};

class Kernel {
public:
    explicit Kernel(platform::Store &store, KernelConfig config = {});

    // The last error the most recent call set, if it set one; reading clears it.
    std::optional<std::uint32_t> TakeLastError();

    // ---- paths (Paths.cpp) ----
    // `name` made absolute against the current directory: `X:\A\B` with `.` and `..` resolved,
    // repeated separators and trailing dots and spaces of a segment dropped. Empty when the name
    // cannot be a path (an empty name, a UNC or device path, a character Windows refuses).
    std::string Absolute(std::string_view name) const;
    GamePath Locate(std::string_view name) const;
    // Where the per-user copy of a game path is (the rules at the top of this file).
    static std::string UserPathOf(std::string_view relative);

    std::string CurrentDirectory() const { return currentDirectory_; }
    bool SetCurrentDirectory(std::string_view name);
    std::string ModuleFileName() const { return config_.gameFolder + config_.exeName; }
    // The absolute path and the offset of its last segment (npos when it ends in `\`).
    std::optional<std::string> FullPathName(std::string_view name, std::size_t *filePart);
    std::optional<std::string> ShortPathName(std::string_view name);
    const std::string &WindowsDirectory() const { return config_.windowsDirectory; }
    const std::string &SystemDirectory() const { return config_.systemDirectory; }
    std::uint32_t DriveType(std::string_view root);
    bool DiskFreeSpace(std::string_view root, std::uint32_t *sectorsPerCluster,
                       std::uint32_t *bytesPerSector, std::uint32_t *freeClusters,
                       std::uint32_t *totalClusters);
    struct VolumeInfo {
        std::string name;
        std::uint32_t serial = 0;
        std::uint32_t maxComponentLength = 0;
        std::uint32_t flags = 0;
        std::string fileSystem;
    };
    std::optional<VolumeInfo> VolumeInformation(std::string_view root);

    // ---- files (Files.cpp) ----
    // A file id (never 0), or 0 with the last error set.
    std::uint32_t CreateFile(std::string_view name, std::uint32_t access, std::uint32_t share,
                             std::uint32_t disposition, std::uint32_t flags);
    bool IsFile(std::uint32_t file) const { return files_.count(file) > 0; }
    bool ReadFile(std::uint32_t file, void *out, std::uint32_t bytes, std::uint32_t *read);
    bool WriteFile(std::uint32_t file, const void *data, std::uint32_t bytes,
                   std::uint32_t *written);
    std::uint32_t SetFilePointer(std::uint32_t file, std::int32_t distance, std::int32_t *high,
                                 std::uint32_t method);
    std::uint32_t GetFileSize(std::uint32_t file, std::uint32_t *high);
    bool GetFileTime(std::uint32_t file, std::uint64_t *creation, std::uint64_t *access,
                     std::uint64_t *write);
    bool SetFileTime(std::uint32_t file, const std::uint64_t *creation,
                     const std::uint64_t *access, const std::uint64_t *write);
    bool FlushFileBuffers(std::uint32_t file);
    bool SetEndOfFile(std::uint32_t file);
    bool CloseFile(std::uint32_t file);

    bool DeleteFile(std::string_view name);
    bool MoveFile(std::string_view from, std::string_view to);
    bool CopyFile(std::string_view from, std::string_view to, bool failIfExists);
    bool CreateDirectory(std::string_view name);
    bool RemoveDirectory(std::string_view name);
    std::uint32_t GetFileAttributes(std::string_view name);
    bool SetFileAttributes(std::string_view name, std::uint32_t attributes);

    // ---- directory search (Find.cpp) ----
    // A search id (never 0) with the first entry, or 0 with the last error set.
    std::uint32_t FindFirstFile(std::string_view pattern, FindData *first);
    bool FindNextFile(std::uint32_t find, FindData *next);
    bool FindClose(std::uint32_t find);
    bool IsFind(std::uint32_t find) const { return finds_.count(find) > 0; }

    // ---- .ini profiles (Ini.cpp) ----
    // Section, key, default and value are the game's bytes; `file` is a path (UTF-8). A file
    // name without a folder is in the Windows directory, as on Windows.
    std::uint32_t GetPrivateProfileString(const char *section, const char *key,
                                          const char *fallback, char *buffer, std::uint32_t size,
                                          std::string_view file);
    std::uint32_t GetPrivateProfileInt(const char *section, const char *key, std::int32_t fallback,
                                       std::string_view file);
    bool WritePrivateProfileString(const char *section, const char *key, const char *value,
                                   std::string_view file);

private:
    struct Entry {
        platform::Root root = platform::Root::kInstall;
        std::string path; // Store path
        platform::FileInfo info;
    };
    struct OpenFile {
        platform::FileHandle handle;
        bool canRead = false;
        bool canWrite = false;
    };
    struct Search {
        std::vector<FindData> entries;
        std::size_t next = 0;
    };

    void SetError(std::uint32_t code) { lastError_ = code; }
    // The entry the game sees at a game path: the per-user copy, else the install's.
    std::optional<Entry> Lookup(std::string_view relative);
    bool IsDirectory(std::string_view relative);
    // ERROR_PATH_NOT_FOUND when the parent folder is missing, else ERROR_FILE_NOT_FOUND.
    std::uint32_t MissingError(std::string_view relative);
    // Makes the per-user folder that will hold `userPath`.
    platform::Status MakeUserParent(std::string_view userPath);
    // Copies an install file to its per-user path, keeping its modification time. A failure sets
    // the last error (a read error stays a read error).
    bool CopyToUser(const Entry &from, std::string_view userPath);
    std::optional<std::string> ReadWhole(const Entry &entry);
    // The merged listing of a game directory, by name without case; the per-user entry wins.
    std::map<std::string, FindData> ListDirectory(std::string_view relative);
    // The game path of an .ini file, and whether the name was bare (see Ini.cpp).
    GamePath IniLocate(std::string_view file, bool *bare) const;

    platform::Store &store_;
    KernelConfig config_;
    std::string currentDirectory_;
    std::optional<std::uint32_t> lastError_;
    std::map<std::uint32_t, OpenFile> files_;
    std::map<std::uint32_t, Search> finds_;
    std::uint32_t nextId_ = 1;
};

// The Win32 error for a Store error, for a call that `writing` or not.
std::uint32_t win32Error(platform::Error error, bool writing);

} // namespace sfcop::hle::kernel
