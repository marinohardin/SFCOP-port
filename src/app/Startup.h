// The client's front door, in the order it runs: the command line (CommandLine.h), the per-user
// data folder (DataDir.h), finding and verifying the install (Install.h), remembering it, and
// installing the fix switches' positions (src/fixes/README.md). main()
// calls startClient() and either returns the exit code it gives or starts the game. With
// --import, startClient imports the game (ImportCommand.h) and stops with its exit code. On a run
// with a window and no game set up, or with --launcher, it opens the launcher first
// (StartupConfig::launcher).
#pragma once

#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

#include "app/CommandLine.h"
#include "app/Files.h"
#include "app/HostPlatform.h"
#include "app/ImportFiles.h"
#include "app/Install.h"

namespace sfcop::app {

// What the launcher (src/app/launcher/) is given when start-up opens it: the run's folders, the
// manifest's place, the folders to suggest, and the file systems to work on.
struct LauncherRequest {
    std::string dataDir;
    std::string gameDir;
    // The hash list; the install manifest sits beside it (Import.h, installManifestBeside).
    std::string hashListPath;
    // The platform's usual places for the install (defaultInstallDirs), to suggest.
    std::vector<std::string> installDirs;
    // --launcher: open on Ready when a game is set up. Otherwise no game was found.
    bool onDemand = false;
    Platform platform = hostPlatform();
    EnvLookup env;
    FileSystem *files = nullptr;
    ImportFiles *importFiles = nullptr;
};

enum class LauncherChoice {
    kPlay,     // start the game: install.txt names it
    kQuit,     // the player closed the launcher; the client exits 0
    kNoWindow, // no window can be opened here: the terminal question is asked instead
    kFailed,   // the launcher could not go on (a bad steps file); the reason is printed; exit 2
};

struct StartupConfig {
    // analysis/results/retail_hashes.sha256, or any list in its format.
    std::string hashListPath;
    // Flags the caller adds to the command line (see ExtraFlag).
    std::vector<ExtraFlag> extraFlags;
    // Install folders tried after the remembered one and before the platform's usual places
    // (the install a build was made from).
    std::vector<std::string> installDirs;
    Platform platform = hostPlatform();
    EnvLookup env = hostEnvironment();
    // The real file system when null.
    FileSystem *files = nullptr;
    // For --import; the real file system when null.
    ImportFiles *importFiles = nullptr;
    // Seconds on a steady clock, for the time --import took; not printed when empty.
    std::function<double()> seconds;
    // Asks the player where the install is, on a run that found none; never on a headless run.
    std::function<std::optional<std::string>()> ask;
    // The launcher's window; empty: the client has no launcher and asks on the terminal.
    // --launcher opens it first. Otherwise, in a package build, a run with a window and no game
    // set up (no --install and no install.txt naming a folder that holds the game) opens it
    // before any other place is tried; in a developer build the order stays --install,
    // install.txt, `installDirs`, the usual places, and the launcher opens only where `ask` would
    // be called. A run with --headless, --input or --oracle never opens it.
    std::function<LauncherChoice(const LauncherRequest &)> launcher;
    // A release package's client (SFCOP_PACKAGE_BUILD), which compiles in no install or hash
    // list: no game set up means the player is prompted and the game imported.
    bool packageBuild = false;
    // Called while the install is hashed.
    ProgressFn progress;
    // std::cout and std::cerr when null.
    std::ostream *out = nullptr;
    std::ostream *err = nullptr;
};

struct Startup {
    // Set when the client stops here: after --help (0), on bad input (2), when the install is
    // refused (3), or when the data folder cannot be made (1). The reason is already printed.
    std::optional<int> exitCode;
    ClientOptions options;
    std::string installDir;
    std::string dataDir;
};

Startup startClient(int argc, const char *const *argv, const StartupConfig &config);

// The configuration of a client run from a terminal: the host platform, the real file system,
// std::cout and std::cerr, and askOnConsole on std::cin.
StartupConfig consoleStartup(std::string hashListPath, std::vector<ExtraFlag> extraFlags = {},
                             std::vector<std::string> installDirs = {});

// An `ask` that prompts on `out` and reads one line from `in`; an empty answer or the end of
// input is no answer. Quotes around the answer are dropped.
std::function<std::optional<std::string>()> askOnConsole(std::istream &in, std::ostream &out);

} // namespace sfcop::app
