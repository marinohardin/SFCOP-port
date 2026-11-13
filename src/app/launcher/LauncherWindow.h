// The launcher's window: SDL3 with Dear ImGui on an SDL renderer, drawing the Launcher's view
// (Launcher.h) and reporting what the player does. It runs on the calling thread, which is the
// client's main thread, before the game starts; the import library's calls run on a worker
// (LauncherJobs.h). It opens its own window and renderer and, before it returns, destroys both,
// shuts Dear ImGui down and releases the video subsystem, so the game's window, GPU device and
// event queue start as they would without it.
#pragma once

#include <SDL3/SDL_dialog.h>

#include <functional>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "app/Startup.h"
#include "app/launcher/LauncherSteps.h"

namespace sfcop::app::launcher {

struct WindowOptions {
    // `--launcher-steps`: played instead of waiting for a player, and named in its errors.
    std::vector<Step> steps;
    std::string stepsName;
};

// kNoWindow when no window can be opened (no display, no video driver); kFailed when a step of
// `options.steps` cannot be done, with the reason on `err`.
LauncherChoice runLauncherWindow(const LauncherRequest &request, const WindowOptions &options,
                                 std::ostream &err);

// What the folder picker sent back. Its callback can run on another thread, on the calling thread
// before the call that opens the picker returns (when SDL fails at once), or, when the window
// closes with the picker open, after the window is gone; so the inbox is shared with it.
struct PickerInbox {
    std::mutex mutex;
    std::optional<std::string> chosen;
    std::optional<std::string> failed;
    bool open = false;
};

// The picker's callback; `userdata` is a heap `std::shared_ptr<PickerInbox>` it takes and frees.
void SDLCALL pickerAnswered(void *userdata, const char *const *filelist, int filter);

// Opens the folder picker through `show` (SDL_ShowOpenFolderDialog in the window) unless one is
// already open. `show` is called with the inbox unlocked, so a callback made before it returns
// does not wait on a lock its own thread holds.
using ShowFolderPicker = std::function<void(SDL_DialogFileCallback callback, void *userdata)>;
void openFolderPicker(const std::shared_ptr<PickerInbox> &inbox, const ShowFolderPicker &show);

// The window's options from `--launcher-steps`: none for an empty path; nothing, with the reason
// on `err`, for a file that cannot be read or holds a wrong step.
std::optional<WindowOptions> readWindowOptions(const std::string &stepsPath, std::ostream &err);

// What StartupConfig::launcher calls in the client: the window, with the steps of `stepsPath` if
// one is given, read only when the launcher opens (kFailed when they are wrong); its messages on
// stderr.
std::function<LauncherChoice(const LauncherRequest &)> windowLauncher(std::string stepsPath);

} // namespace sfcop::app::launcher
