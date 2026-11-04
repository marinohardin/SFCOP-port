// The native client's entry point.
//
// startClient (Startup.h) takes the command line of DESIGN §6.8, finds or makes the per-user data
// folder, and finds, hash-verifies and remembers the install. Until Wave 1 replaces it, the game
// then runs on the temporary boot platform (src/app/boot/), the P1-2 spike host on pcrecomp's
// win32hle: the verified install and the client's options become that platform's arguments.

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "ExitCodes.h"
#include "Startup.h"
#include "boot/BootHost.h"
#include "sfcop_build_config.h"

namespace {

// The temporary boot platform's own flags, until Wave 1. --help lists them and startClient hands
// them back unchanged in passThrough.
const std::vector<sfcop::app::ExtraFlag> kBootFlags = {
    {"--game", "DIR", "a writable copy of the install (default: the build's copy)"},
    {"--trace", "", "log every Win32 call on stderr"},
    {"--silent", "", "no sound"},
    {"--shots", "DIR", "write screenshots into DIR"},
    {"--shot-every", "S", "seconds between screenshots (default 5)"},
    {"--dry-run", "", "map and bind the exe, then stop"},
};

bool isCount(const std::string &text) {
    if (text.empty() || text[0] == '-') {
        return false;
    }
    char *end = nullptr;
    errno = 0;
    const unsigned long value = std::strtoul(text.c_str(), &end, 10);
    return errno == 0 && *end == '\0' && value <= 86400UL;
}

bool sameFolder(const std::string &a, const std::string &b) {
    std::error_code error;
    return std::filesystem::equivalent(a, b, error) && !error;
}

}  // namespace

int main(int argc, char **argv) {
    using namespace sfcop::app;
    const Startup start = startClient(
        argc, argv, consoleStartup(SFCOP_HASH_LIST, kBootFlags, {SFCOP_INSTALL_DIR}));
    if (start.exitCode) {
        return *start.exitCode;
    }

    // The boot platform's flags, out of passThrough. The parser has already checked that every
    // flag that takes a value has one.
    std::string game = SFCOP_DEFAULT_GAME_DIR;
    bool dryRun = false;
    std::vector<std::string> pass;
    const std::vector<std::string> &through = start.options.passThrough;
    for (std::size_t i = 0; i < through.size(); i++) {
        const std::string &arg = through[i];
        const bool hasValue = i + 1 < through.size();
        if (arg == "--game" && hasValue) {
            game = through[++i];
        } else if (arg == "--dry-run") {
            dryRun = true;
        } else if (arg == "--shot-every" && hasValue) {
            const std::string &value = through[++i];
            if (!isCount(value)) {
                std::fprintf(stderr, "sfcop: --shot-every takes a number of seconds, not '%s'\n",
                             value.c_str());
                return kExitBadInput;
            }
            pass.push_back(arg);
            pass.push_back(value);
        } else if (arg == "--shots" && hasValue) {
            pass.push_back(arg);
            pass.push_back(through[++i]);
        } else {
            pass.push_back(arg);  // --trace, --silent
        }
    }
    if (sameFolder(game, start.installDir)) {
        std::fprintf(stderr,
                     "sfcop: %s is the install itself; the boot platform writes into its game "
                     "folder, so give a copy\n",
                     game.c_str());
        return kExitBadInput;
    }

    // --windowed and --fullscreen are parsed and kept in start.options.window; the boot platform
    // always opens a window (F11 toggles) until Wave 1.
    std::vector<std::string> bootArgs = {"sfcop", "--game", game};
    if (!dryRun) {
        bootArgs.emplace_back("--run");
    }
    if (start.options.headless) {
        bootArgs.emplace_back("--headless");
    }
    if (start.options.watchdogSeconds > 0) {
        bootArgs.emplace_back("--watchdog");
        bootArgs.push_back(std::to_string(start.options.watchdogSeconds));
    }
    bootArgs.insert(bootArgs.end(), pass.begin(), pass.end());
    std::vector<char *> bootArgv;
    for (std::string &a : bootArgs) {
        bootArgv.push_back(a.data());
    }
    bootArgv.push_back(nullptr);
    return sfcop_boot_main(static_cast<int>(bootArgs.size()), bootArgv.data());
}
