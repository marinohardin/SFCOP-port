// The client's front door, in the order it runs: the command line (CommandLine.h), the per-user
// data folder (DataDir.h), finding and verifying the install (Install.h), remembering it, and
// installing the fix switches' positions (src/fixes/README.md). main()
// calls startClient() and either returns the exit code it gives or starts the game.
#pragma once

#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

#include "app/CommandLine.h"
#include "app/Files.h"
#include "app/HostPlatform.h"
#include "app/Install.h"

namespace sfcop::app {

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
    // Asks the player where the install is, on a run that found none; never on a headless run.
    std::function<std::optional<std::string>()> ask;
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
