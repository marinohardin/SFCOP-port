// What the launcher's window is set up from (Launcher.h): start-up's folders turned into the
// suggestions, the game in use and the folder picker's availability, and install.txt written.
// No window here, so all of it is tested (LauncherSetupTest.cpp).
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "app/Files.h"
#include "app/HostPlatform.h"
#include "app/Startup.h"
#include "app/launcher/Launcher.h"

namespace sfcop::app::launcher {

// The folders `dirs` names that exist, in order and without repeats, each marked when it holds
// the game's executable itself.
std::vector<Suggestion> suggestionsFrom(const std::vector<std::string> &dirs,
                                        const FileSystem &files);

// The game a run would start from now: what install.txt names, when it holds the game. For the
// client's own copy (`gameDir`), the import record's source and counts, and `modifiedTime` of the
// record (empty when unknown) as when it was made.
std::optional<GameInUse> gameInUseOf(const std::string &dataDir, const std::string &gameDir,
                                     const FileSystem &files,
                                     const std::function<std::string(const std::string &)>
                                         &modifiedTime);

// Whether the system's folder picker can open: always on macOS and Windows; on Linux SDL needs the
// desktop portal (over the session bus) or zenity, so without a session bus and without zenity on
// PATH it cannot. `onPath` says whether a program is on PATH.
FolderPicker folderPickerOn(Platform platform, const EnvLookup &env,
                            const std::function<bool(const std::string &)> &onPath);

// The launcher's setup for `request`, with the manifest read beside the hash list (an unreadable
// one is a manifest error, which every import and check then reports as the client's fault).
Setup setupFor(const LauncherRequest &request, FolderPicker picker);

} // namespace sfcop::app::launcher
