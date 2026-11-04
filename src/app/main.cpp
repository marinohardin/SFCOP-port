// The native client's entry point.
//
// Until Wave 1 replaces it, the game runs on the temporary boot platform (src/app/boot/), the
// P1-2 spike host on pcrecomp's win32hle. This file takes the client's command line, turns it
// into the boot platform's, and hands over. The full command line of DESIGN §6.8 (--install,
// --data, --fix, --oracle) and install verification belong to the application skeleton (C4).

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "boot/BootHost.h"
#include "sfcop_build_config.h"

namespace {

constexpr int kExitBadInput = 2;

void printUsage(std::FILE *out) {
    std::fprintf(out,
                 "usage: sfcop [--game DIR] [--headless] [--watchdog S] [--windowed|--fullscreen]\n"
                 "             [--trace] [--silent] [--shots DIR] [--shot-every S] [--dry-run]\n"
                 "\n"
                 "  --game DIR      a writable copy of the install (default: the build's copy,\n"
                 "                  %s)\n"
                 "  --headless      no window, no sound\n"
                 "  --watchdog S    stop after S seconds; exit 0 if the boot reached the CD-key\n"
                 "                  prompt, 1 if not (0: no watchdog)\n"
                 "  --windowed, --fullscreen\n"
                 "                  accepted; the boot platform always opens a window (F11 toggles)\n"
                 "  --trace         log every Win32 call on stderr\n"
                 "  --silent        no sound\n"
                 "  --shots DIR     write screenshots into DIR; --shot-every S seconds (default 5)\n"
                 "  --dry-run       map and bind the exe, then stop\n",
                 SFCOP_DEFAULT_GAME_DIR);
}

bool isCount(const char *text) {
    if (*text == '\0') {
        return false;
    }
    char *end = nullptr;
    errno = 0;
    const unsigned long value = std::strtoul(text, &end, 10);
    return errno == 0 && *end == '\0' && text[0] != '-' && value <= 86400UL;
}

bool sameFolder(const std::string &a, const std::string &b) {
    std::error_code error;
    return std::filesystem::equivalent(a, b, error) && !error;
}

}  // namespace

int main(int argc, char **argv) {
    std::string game = SFCOP_DEFAULT_GAME_DIR;
    std::vector<std::string> pass;
    bool dryRun = false;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--help" || arg == "-h") {
            printUsage(stdout);
            return 0;
        } else if (arg == "--game" && hasValue) {
            game = argv[++i];
        } else if ((arg == "--watchdog" || arg == "--shot-every") && hasValue) {
            if (!isCount(argv[i + 1])) {
                std::fprintf(stderr, "sfcop: %s takes a number of seconds, not '%s'\n", arg.c_str(),
                             argv[i + 1]);
                return kExitBadInput;
            }
            pass.push_back(arg);
            pass.push_back(argv[++i]);
        } else if (arg == "--shots" && hasValue) {
            pass.push_back(arg);
            pass.push_back(argv[++i]);
        } else if (arg == "--headless" || arg == "--trace" || arg == "--silent") {
            pass.push_back(arg);
        } else if (arg == "--windowed" || arg == "--fullscreen") {
            // DESIGN §6.8 flags; the boot platform decides the window itself.
        } else if (arg == "--dry-run") {
            dryRun = true;
        } else {
            std::fprintf(stderr, "sfcop: unknown or incomplete option '%s'\n\n", arg.c_str());
            printUsage(stderr);
            return kExitBadInput;
        }
    }
    if (sameFolder(game, SFCOP_INSTALL_DIR)) {
        std::fprintf(stderr, "sfcop: %s is the install itself; the boot platform writes into its "
                             "game folder, so give a copy\n",
                     game.c_str());
        return kExitBadInput;
    }

    std::vector<std::string> bootArgs = {"sfcop", "--game", game};
    if (!dryRun) {
        bootArgs.emplace_back("--run");
    }
    bootArgs.insert(bootArgs.end(), pass.begin(), pass.end());
    std::vector<char *> bootArgv;
    for (std::string &a : bootArgs) {
        bootArgv.push_back(a.data());
    }
    bootArgv.push_back(nullptr);
    return sfcop_boot_main(static_cast<int>(bootArgs.size()), bootArgv.data());
}
