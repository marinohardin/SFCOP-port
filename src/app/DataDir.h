// The per-user data folder (DESIGN §5.3, §6.6): where it is on each platform, its subfolders,
// and the install path a previous run remembered there. Everything the port writes goes here;
// the install is never written.
#pragma once

#include <optional>
#include <string>

#include "app/Files.h"
#include "app/HostPlatform.h"

namespace sfcop::app {

// The subfolders of DESIGN §6.6, created on first use. registry.tsv is a file Store writes.
inline constexpr const char *kDataSubdirs[] = {"settings", "saves", "replays", "logs", "mods"};

// Where the install found on the first run is remembered: one line, the install's path.
inline constexpr const char kRememberedInstallFile[] = "install.txt";

struct DataDirResult {
    std::optional<std::string> dir;
    // For the player when the folder cannot be worked out.
    std::string error;
};

// --data when given; otherwise ~/Library/Application Support/SFCOP on macOS, %APPDATA%\SFCOP on
// Windows, $XDG_DATA_HOME/SFCOP on Linux (~/.local/share/SFCOP when XDG_DATA_HOME is unset, empty
// or relative, as the XDG base directory rules say).
DataDirResult resolveDataDir(Platform platform, const EnvLookup &env,
                             const std::optional<std::string> &flag);

// Creates the folder and its subfolders when missing. Empty on success, else what failed.
std::string ensureDataDir(const std::string &dir, FileSystem &files);

std::optional<std::string> rememberedInstall(const std::string &dataDir, const FileSystem &files);
bool rememberInstall(const std::string &dataDir, const std::string &installDir, FileSystem &files);

} // namespace sfcop::app
