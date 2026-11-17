// The File menu of the running game (macOS menu bar): which folders its items show and what an
// item does. The menu itself is the host's (FileMenuHost_<platform>); everything here is plain
// and tested on every platform.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace sfcop::app {

struct FileMenuEntry {
    std::string title;
    // The folder the item shows in the file manager.
    std::string path;
};

// "Show Game Files in Finder" shows the data folder the client is using (the resolved one, so
// --data is honoured), not its `game` subfolder: the imported copy, install.txt, the settings and
// the saves sit side by side there, so one folder holds everything a fresh install deletes.
// "Show Logs in Finder" shows `logsDir`. An entry with an empty path is left out.
std::vector<FileMenuEntry> fileMenuEntries(const std::string &dataDir, const std::string &logsDir);

// Shows `path` in the platform's file manager; false when it could not be shown.
using RevealFn = std::function<bool(const std::string &path)>;

// What choosing entry `index` does: reveals its path through `reveal`. False for an index outside
// the list or when `reveal` fails.
bool activateFileMenuEntry(const std::vector<FileMenuEntry> &entries, std::size_t index,
                           const RevealFn &reveal);

// Defined once per platform, in FileMenuHost_<platform>.
// Shows `path` in the file manager (Finder on macOS); false where there is none or on failure.
bool revealInFileManager(const std::string &path);
// Adds a File menu with `entries` to the application's menu bar, after SDL's default menus. Call
// after the window system is up (the Window's creation). A no-op off macOS.
void installFileMenu(const std::vector<FileMenuEntry> &entries);
// Call once per pump of the main thread's events, after installFileMenu: finishes the install when
// the window system was not up at that call, and puts the File menu back if the main menu was
// replaced. Cheap once the menu is in place; logs once, with the number of pumps it took. A no-op
// off macOS.
void pumpFileMenu();

} // namespace sfcop::app
