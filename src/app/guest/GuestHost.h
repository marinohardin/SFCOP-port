// GuestHost: runs the lifted game as a component of the client (DESIGN §7.3).
//
// It maps the original exe at its base, binds its imports to the HLE modules, gives every guest
// thread its static TLS, and enters the C runtime's start-up, which calls WinMain. The modules
// work on the components the client constructs (src/app/main.cpp); this host attaches them first.
//
// Threads: the game's own threads stay as they are, each on a Threads thread of its own
// (hle/kernel32rt); one machine lock lets one of them run guest code at a time. The thread that
// calls Run() is the game's main thread and the platform's: the window's events are pumped there,
// whenever the game's message queue runs empty (DESIGN §7.3).
//
// Only one GuestHost runs in a process, and Run() does not return: the game ends the process as
// ExitProcess does, with the exit code mapped to the client's (0 stays 0, anything else is 1).
#pragma once

#include "platform/Audio/Audio.h"
#include "platform/Clock/Clock.h"
#include "platform/Gpu/Gpu.h"
#include "platform/Input/Input.h"
#include "platform/Store/Store.h"
#include "platform/Threads/Threads.h"
#include "platform/Transport/Transport.h"
#include "platform/Window/Window.h"

#include <cstdint>
#include <functional>
#include <string>

namespace sfcop::guest {

// The client's components, constructed by main.cpp; they outlive the run.
struct Components {
    platform::Window &window;
    platform::Gpu &gpu;
    platform::Input &input;
    platform::Audio &audio;
    platform::Store &store;
    platform::Transport &transport;
    platform::Clock &clock;
    platform::Threads &threads;
};

struct Options {
    std::string installDir; // the verified install; mapped as drive C: and never written
    std::string dataDir;    // the per-user folder: settings, saves, logs
    std::string logsDir;    // non-empty: where the game's .txt and .log files land instead of
                            // logs/ under dataDir (--logs; hle/kernel LogsStore.h)
    std::string exeName = "StarFleetOP.exe";
    std::string arguments;  // after the exe's name on the guest's command line
    std::string title = "Starfleet Command: Orion Pirates";
    platform::WindowMode windowMode = platform::WindowMode::kWindowed;
    std::uint32_t windowWidth = 0;  // --windowed WxH: the frame's first size in points, the
    std::uint32_t windowHeight = 0; // game scaled into it; 0: the game's own size
    bool headless = false;  // the stub Window and Gpu: no window, nothing shown
    bool silent = false;    // no sound device
    bool trace = false;     // one line per Win32 call, and the exception dispatch's own lines
    bool dryRun = false;    // map and bind the exe, then return 0 without entering it
    unsigned watchdogSeconds = 0; // 0: none; else the run ends then, 0 if the goal mark was reached
    std::string shotsDir;         // non-empty: a BMP of the display every shotEverySeconds (not headless)
    double shotEverySeconds = 5.0;
    // --render-scale: the Direct3D targets' render scale option (hle/ddraw4 DirectDraw::
    // SetRenderScaleOption): 0 from the window, 1 the game's own size, N > 1 at most N. main.cpp
    // sets 1 for --oracle and --record-trace, whose frames must be the game's own.
    std::uint8_t renderScale = 0;
    // Whether the window's events are also read inside the game's calls, so a long load keeps the
    // window responsive (src/app/guest/README.md, "The pump inside the game's calls"). main.cpp
    // clears it for --input, --oracle, --record-trace and --headless: those runs count frames,
    // and their window is read only when the game peeks.
    bool pumpDuringLoads = true;
    // Non-empty: called on the main thread after each turn of the pump that read no event from
    // the Window, the turn after which PeekMessageA finds the queue empty and the game runs a
    // frame. A turn that read an event (the OS's or an input script's) is not a frame, so events
    // that only a window gets do not change the count. The fixed-step clock advances on it
    // (platform/Clock/FixedStepClock.h).
    std::function<void()> onFrame;
};

// Maps, binds and enters the game. Returns only for a dry run or when the exe cannot be mapped
// (kExitSuccess or kExitFailure); otherwise the game ends the process.
int Run(const Options &options, Components &components);

// The game-specific part (GuestGame.cpp for the original; the guest tests have their own): the
// built-in DLLs it loads, the functions the lift could not see, and the marks. Called by Run()
// after the modules' tables are registered and before the exe is bound.
void InstallGameSpecifics();

// For InstallGameSpecifics: a mark was reached that a watchdog run counts as success (the CD-key
// prompt). Safe from any thread.
void ReachedGoal();

// Seconds since Run() started, for log lines.
double Elapsed();

} // namespace sfcop::guest
