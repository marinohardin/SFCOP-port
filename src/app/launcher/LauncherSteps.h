// A steps file for the launcher's window (`--launcher-steps FILE`): drives the screens the way a
// player would, for captures and checks of the whole flow, without the system's folder picker,
// which cannot be scripted. One step per line; blank lines and lines starting with '#' are
// skipped:
//
//   wait welcome|working|result|ready   until that screen shows (at most 10 minutes)
//   frames N                            let N frames pass (1 to 100000)
//   choose PATH                         a folder chosen, as from the picker or dropped
//   type TEXT                           TEXT in the path field
//   press NAME                          a button: use-path, cancel, play, check, change, back,
//                                       another, retry, quit
//   close                               the window's close button
//   shot FILE                           the next frame drawn, written to FILE as a BMP
//
// A press of a button the screen does not show is an error, as is a wait that runs out; the
// launcher then ends with the line named. A file that ends without `press play`, `press quit` or
// `close` leaves the window open for a player.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "app/launcher/Launcher.h"

namespace sfcop::app::launcher {

enum class StepKind { kWait, kFrames, kChoose, kType, kPress, kClose, kShot };

struct Step {
    StepKind kind = StepKind::kWait;
    int line = 0;
    Screen screen = Screen::kWelcome; // kWait
    unsigned frames = 0;              // kFrames
    Action action = Action::kQuit;    // kPress
    std::string text;                 // kChoose, kType, kShot
};

struct ParsedSteps {
    std::vector<Step> steps;
    // Empty when the file is good; otherwise "line N: what is wrong".
    std::string error;
};

ParsedSteps parseLauncherSteps(const std::string &text);

} // namespace sfcop::app::launcher
