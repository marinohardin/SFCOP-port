// The native client's entry point.
//
// startClient (Startup.h) takes the command line of DESIGN §6.8, finds or makes the per-user data
// folder, and finds, hash-verifies and remembers the install. The client then constructs the
// platform components and runs the lifted game on them in the guest host (src/app/guest/).

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "ExitCodes.h"
#include "Startup.h"
#include "guest/GuestHost.h"
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
    guest::Components components{*window, *gpu,       *input, *audio,
                                 *store,  *transport, *clock, *threads};
    return guest::Run(options, components);
}
