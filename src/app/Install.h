// Finding the player's install and verifying it against the hash list (DESIGN §5.1).
//
// The hash list is in sha256sum's format: per line, 64 hex digits, two spaces (or a space and
// '*'), and a path relative to the list's root with '/' between components. Blank lines and
// lines starting with '#' are skipped. The install is only ever read.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "app/Files.h"
#include "app/HostPlatform.h"

namespace sfcop::app {

// The file that makes a folder an install, and the first file verified.
inline constexpr const char kGameExe[] = "StarFleetOP.exe";

struct HashEntry {
    // 64 lowercase hex digits.
    std::string sha256;
    // Relative to the root, '/' between components, never leaving the root.
    std::string path;
    // 1-based line in the hash list.
    int line = 0;
};

struct HashList {
    std::vector<HashEntry> entries;
    // Empty when the list is good; otherwise "line N: what is wrong".
    std::string error;

    bool ok() const { return error.empty(); }
};

HashList parseHashList(const std::string &text);

// The entries of the game install within a list whose root is above it (the repository's list
// also names the server kit). The install is the folder of the kGameExe entry with the most
// entries under it; those entries come back relative to it, in list order. An error when the
// list names no kGameExe.
HashList installEntries(const HashList &list);

// The boot stub beside the game executable; the client never runs it.
inline constexpr const char kBootStubExe[] = "SFCOP.exe";

// The entries a run verifies before it starts the game: the install's entries without the boot
// stub and the SDK's static libraries (*.lib), which the game never reads and an imported copy
// (Import.h) leaves out. In list order.
std::vector<HashEntry> runtimeEntries(const std::vector<HashEntry> &entries);

struct VerifyProgress {
    std::size_t fileIndex = 0; // 0-based, in list order
    std::size_t fileCount = 0;
    std::string path;
    std::uint64_t bytesDone = 0; // over all files
    std::uint64_t bytesTotal = 0;
};

using ProgressFn = std::function<void(const VerifyProgress &)>;

enum class VerifyStatus { kAccepted, kMissing, kMismatch, kUnreadable };

struct VerifyResult {
    VerifyStatus status = VerifyStatus::kAccepted;
    // The first file at fault, as the hash list names it.
    std::string path;
    std::string root;
    // When accepted: the files that differ from the list but were allowed to, in list order.
    std::vector<std::string> differing;

    bool accepted() const { return status == VerifyStatus::kAccepted; }
    // 0 when accepted, 3 (install refused) otherwise.
    int exitCode() const;
    // For the player: which file is wrong and what to do about it.
    std::string message() const;
};

// Hashes every entry under `root`, in list order, reading each file in pieces, and stops at the
// first file that is missing, unreadable or different. `progress` is called after every piece.
// A file `mayDiffer` names (compared ignoring case) is accepted with any content and listed in
// `differing` when it is different; it must still be there and readable.
VerifyResult verifyInstall(const std::string &root, const std::vector<HashEntry> &entries,
                           const FileSystem &files, const ProgressFn &progress = {},
                           const std::vector<std::string> &mayDiffer = {});

enum class InstallSource { kNone, kFlag, kRemembered, kDefault, kAsked };

struct LocateRequest {
    // --install: when given, the only candidate.
    std::optional<std::string> flag;
    // The path a previous run remembered; skipped when it no longer holds an install.
    std::optional<std::string> remembered;
    // Tried in order after the remembered one.
    std::vector<std::string> defaults;
    // Asks the player once when nothing else holds an install; may be empty.
    std::function<std::optional<std::string>()> ask;
};

struct LocateResult {
    std::optional<std::string> dir;
    InstallSource source = InstallSource::kNone;
    // For the player when nothing was found.
    std::string error;
};

bool holdsInstall(const std::string &dir, const FileSystem &files);
LocateResult locateInstall(const LocateRequest &request, const FileSystem &files);

// The usual places for the install on each platform: Program Files on Windows, a Wine prefix's
// drive_c elsewhere.
std::vector<std::string> defaultInstallDirs(Platform platform, const EnvLookup &env);

} // namespace sfcop::app
