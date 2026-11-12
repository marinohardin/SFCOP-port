// The per-user data folder (DESIGN §5.3, §6.6): where it is on each platform, its subfolders,
// and the install path a previous run remembered there. Everything the port writes goes here;
// the install is never written.
#pragma once

#include <optional>
#include <string>

#include "app/Files.h"
#include "app/HostPlatform.h"
#include "platform/Store/Store.h"

namespace sfcop::app {

// The subfolders of DESIGN §6.6, created on first use: Store's user folders, under Store's names,
// so the two lists cannot drift apart. registry.tsv is a file Store writes.
inline constexpr const char *kDataSubdirs[] = {
    platform::userFolderName(platform::UserFolder::kSettings),
    platform::userFolderName(platform::UserFolder::kSaves),
    platform::userFolderName(platform::UserFolder::kReplays),
    platform::userFolderName(platform::UserFolder::kLogs),
    platform::userFolderName(platform::UserFolder::kMods),
};

// Where the install found on the first run is remembered: one line, the install's path, or
// kImportedGame for the copy `--import` made (gameDirFor).
inline constexpr const char kRememberedInstallFile[] = "install.txt";

// The imported copy of the game (Import.h): the folder's name under the data folder, and the
// value install.txt holds for it.
inline constexpr const char kImportedGame[] = "game";

// Where the player names the folder of translated mission modules, once, for every later run
// (a run started from Finder has no environment): one line, the folder's path, absolute or
// relative to the data folder. The client only reads it; SFCOP_MISSION_MODULES wins over it.
inline constexpr const char kRememberedMissionModulesFile[] = "mission-modules.txt";

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

// Where `--import` puts the game: `game` in the data folder; on Windows, when no --data is given,
// %LOCALAPPDATA%\SFCOP\game instead, so that a roaming profile does not carry 750 MB of game
// files (the data folder there is the roaming %APPDATA%\SFCOP). `dataFlag`: --data was given.
std::string gameDirFor(Platform platform, const EnvLookup &env, bool dataFlag,
                       const std::string &dataDir);

// The first line of install.txt as written.
std::optional<std::string> rememberedInstall(const std::string &dataDir, const FileSystem &files);
// install.txt's install as a folder: kImportedGame is `gameDir`, an absolute path is kept, and
// any other value is taken relative to the data folder.
std::optional<std::string> rememberedInstallDir(const std::string &dataDir,
                                                const std::string &gameDir,
                                                const FileSystem &files);
bool rememberInstall(const std::string &dataDir, const std::string &installDir, FileSystem &files);

// The folder kRememberedMissionModulesFile names, made absolute against `dataDir`; nothing when
// the file is missing or its first line is empty.
std::optional<std::string> rememberedMissionModules(const std::string &dataDir,
                                                    const FileSystem &files);

} // namespace sfcop::app
