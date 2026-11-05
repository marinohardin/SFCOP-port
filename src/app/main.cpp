// The native client's entry point.
//
// startClient (Startup.h) takes the command line of DESIGN §6.8, finds or makes the per-user data
// folder, and finds, hash-verifies and remembers the install. The client then constructs the
// platform components and runs the lifted game on them in the guest host (src/app/guest/).
// With --input, the client's Window is wrapped so the steps file plays into the game through the
// path real input takes (src/app/input/).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "ExitCodes.h"
#include "Startup.h"
#include "guest/GuestHost.h"
#include "input/Marks.h"
#include "input/ScriptedGpu.h"
#include "input/ScriptedWindow.h"
#include "sfcop_build_config.h"

namespace {

// The client's own flags beyond DESIGN §6.8. --help lists them and startClient hands them back
// unchanged in passThrough.
const std::vector<sfcop::app::ExtraFlag> kClientFlags = {
    {"--trace", "", "log every Win32 call and every guest exception on stderr"},
    {"--silent", "", "no sound"},
    {"--dry-run", "", "map and bind the exe, then stop"},
    {"--shots", "DIR", "write a BMP of the game's display into DIR (needs a window)"},
    {"--shot-every", "S", "seconds between shots (default 5; with --shots)"},
};

// Reads and checks the --input steps file; on a bad file prints why and returns nothing.
std::optional<sfcop::app::input::InputScript> readInputScript(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    if (!file || !(text << file.rdbuf())) {
        std::fprintf(stderr, "sfcop: cannot read the input script %s\n", path.c_str());
        return std::nullopt;
    }
    sfcop::app::input::ParsedScript parsed = sfcop::app::input::parseInputScript(text.str());
    if (parsed.error) {
        std::fprintf(stderr, "sfcop: input script %s, %s\n", path.c_str(),
                     sfcop::app::input::describe(*parsed.error).c_str());
        return std::nullopt;
    }
    return std::move(parsed.script);
}

// `waitfile`'s path: a Windows path (a drive or a backslash) is looked up by its last name in the
// data folder, where the game's files are written; a relative path is taken from the data folder.
bool scriptFileExists(const std::string &dataDir, const std::string &path) {
    std::filesystem::path host;
    const bool windowsPath =
        path.find('\\') != std::string::npos || (path.size() >= 2 && path[1] == ':');
    if (windowsPath) {
        host = std::filesystem::path(dataDir) / path.substr(path.find_last_of("\\:/") + 1);
    } else if (std::filesystem::path(path).is_absolute()) {
        host = path;
    } else {
        host = std::filesystem::path(dataDir) / path;
    }
    std::error_code error;
    return std::filesystem::exists(host, error);
}

}  // namespace

int main(int argc, char **argv) {
    using namespace sfcop;
    using namespace sfcop::app;
    const Startup start = startClient(
        argc, argv, consoleStartup(SFCOP_HASH_LIST, kClientFlags, {SFCOP_INSTALL_DIR}));
    if (start.exitCode) {
        return *start.exitCode;
    }

    guest::Options options;
    options.installDir = start.installDir;
    options.dataDir = start.dataDir;
    options.headless = start.options.headless;
    options.watchdogSeconds = start.options.watchdogSeconds;
    options.windowMode = start.options.window == WindowMode::kFullscreen
                             ? platform::WindowMode::kFullscreen
                             : platform::WindowMode::kWindowed;
    const std::vector<std::string> &extras = start.options.passThrough;
    for (std::size_t i = 0; i < extras.size(); ++i) {
        const std::string &flag = extras[i];
        options.trace = options.trace || flag == "--trace";
        options.silent = options.silent || flag == "--silent";
        options.dryRun = options.dryRun || flag == "--dry-run";
        if (flag == "--shots" && i + 1 < extras.size()) {
            options.shotsDir = extras[++i];
        } else if (flag == "--shot-every" && i + 1 < extras.size()) {
            const double seconds = std::atof(extras[++i].c_str());
            if (seconds > 0) {
                options.shotEverySeconds = seconds;
            }
        }
    }

    // Headless: the stub Window and Gpu, so nothing is shown and no display is needed.
    std::unique_ptr<platform::Window> window =
        options.headless ? platform::makeWindowStub() : platform::makeWindow();
    std::unique_ptr<platform::Gpu> gpu =
        options.headless ? platform::makeGpuStub() : platform::makeGpu();
    std::unique_ptr<platform::Input> input = platform::makeInput();
    std::unique_ptr<platform::Audio> audio = options.headless || options.silent
                                                 ? platform::makeAudioStub()
                                                 : platform::makeAudio();
    std::unique_ptr<platform::Store> store = platform::makeStore(options.installDir, options.dataDir);
    std::unique_ptr<platform::Transport> transport = platform::makeTransport();
    std::unique_ptr<platform::Clock> clock = platform::makeClock();
    std::unique_ptr<platform::Threads> threads = platform::makeThreads();
    if (!window || !gpu || !input || !audio || !store || !transport || !clock || !threads) {
        std::fprintf(stderr, "sfcop: a platform component could not be created\n");
        return kExitFailure;
    }

    // --input: the steps file plays through a wrapper around the window, on this thread, which is
    // the game's main thread (guest::Run is called from here).
    std::optional<app::input::ScriptPlayer> player;
    std::optional<app::input::ScriptedWindow> scripted;
    std::optional<app::input::ScriptedGpu> scriptedGpu;
    if (start.options.inputScript) {
        std::optional<app::input::InputScript> script = readInputScript(*start.options.inputScript);
        if (!script) {
            return kExitBadInput;
        }
        const std::string dataDir = options.dataDir;
        app::input::PlayerEnvironment environment;
        environment.fileExists = [dataDir](const std::string &path) {
            return scriptFileExists(dataDir, path);
        };
        environment.markReached = [](const std::string &mark) {
            return app::input::MarkReached(mark);
        };
        environment.log = [](const std::string &line) {
            std::fprintf(stderr, "[input] %s\n", line.c_str());
        };
        player.emplace(std::move(*script), std::move(environment));
        scripted.emplace(*window, *player, *clock, [](const app::input::PlayerFailure &failure) {
            std::fprintf(stderr, "sfcop: input script line %u: %s\n", failure.line,
                         failure.message.c_str());
            std::fflush(stderr);
            // _Exit, not exit: this callback runs inside the host's pump, on the game's main
            // thread, so running the exit handlers would unwind through guest frames and static
            // destructors that other threads still use. Nothing buffered may be lost, so flush.
            std::fflush(stdout);
            std::_Exit(kExitBadInput);
        });
        scripted->BindToCallingThread();
        scriptedGpu.emplace(*gpu, *scripted); // attaches the Gpu to the real window
        if (options.headless) {
            // The host attaches Input only to a window it opens; headless, the script's events
            // reach DirectInput through this attachment (the stub window refuses it, unharmed).
            static_cast<void>(scripted->AttachInput(*input));
        }
    }
    platform::Window &gameWindow = scripted ? static_cast<platform::Window &>(*scripted) : *window;
    platform::Gpu &gameGpu = scriptedGpu ? static_cast<platform::Gpu &>(*scriptedGpu) : *gpu;
    guest::Components components{gameWindow, gameGpu,    *input, *audio,
                                 *store,     *transport, *clock, *threads};
    return guest::Run(options, components);
}
