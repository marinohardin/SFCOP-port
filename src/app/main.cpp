// The native client's entry point.
//
// startClient (Startup.h) takes the command line of DESIGN §6.8, finds or makes the per-user data
// folder, and finds, hash-verifies and remembers the install. The client then constructs the
// platform components and runs the lifted game on them in the guest host (src/app/guest/).
// With --input, the client's Window is wrapped so the steps file plays into the game through the
// path real input takes (src/app/input/). With --oracle TRACE (or --record-trace OUT) the replay
// oracle waits for the mission's END on a thread of its own, makes the trace and compares it
// (src/app/oracle/OracleRun.h), then ends the process with the comparator's status.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "ExitCodes.h"
#include "Startup.h"
#include "guest/GuestHost.h"
#include "hle/kernel32rt/Runtime.h"
#include "input/Marks.h"
#include "input/ScriptedGpu.h"
#include "input/ScriptedWindow.h"
#include "oracle/OracleRun.h"
#include "platform/Clock/FixedStepClock.h"
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
    {"--record-trace", "OUT",
     "play the mission to END and write its trace to OUT, <dir>/<mission>/<input>.trace "
     "(needs --input; --oracle compares as well)"},
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
// logs folder, where the game's .txt and .log files are written (--logs, else logs/ under the
// data folder), then in the data folder; a relative path is taken from the data folder.
bool scriptFileExists(const std::string &dataDir, const std::string &logsDir,
                      const std::string &path) {
    std::filesystem::path host;
    const bool windowsPath =
        path.find('\\') != std::string::npos || (path.size() >= 2 && path[1] == ':');
    std::error_code error;
    if (windowsPath) {
        const std::string name = path.substr(path.find_last_of("\\:/") + 1);
        if (std::filesystem::exists(std::filesystem::path(logsDir) / name, error)) {
            return true;
        }
        host = std::filesystem::path(dataDir) / name;
    } else if (std::filesystem::path(path).is_absolute()) {
        host = path;
    } else {
        host = std::filesystem::path(dataDir) / path;
    }
    return std::filesystem::exists(host, error);
}

// The replay oracle around the game: on its own thread, waits for the mission's END in the log,
// makes the trace, compares when an expected trace was given, and ends the process with the
// result. It owns the run's deadline: the guest host's watchdog is off on an oracle run, since
// that one counts a run that reached the menu as a success.
struct OracleJob {
    std::string logPath;       // <logs>/OracleTrace.txt
    std::string outPath;       // the trace written
    std::optional<std::string> expectedPath;
    sfcop::app::oracle::TraceNames names;
    sfcop::app::oracle::Tools tools;
    std::string exePath;
    std::string sourceDir;
    std::string scratchDir;
    unsigned deadlineSeconds = 0;
    const sfcop::platform::FixedStepClock *fixedStep = nullptr; // the run's clock, if fixed
};

[[noreturn]] void endOracle(int code) {
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(code); // as the watchdog and the input script's failure do: the game's threads
                      // are not unwound
}

void runOracle(const OracleJob &job) {
    using namespace sfcop::app::oracle;
    const auto started = std::chrono::steady_clock::now();
    const auto clockMs = [started] {
        return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now() - started)
                                              .count());
    };
    const auto sleepMs = [](std::uint32_t ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    };
    const std::uint32_t deadlineMs = job.deadlineSeconds * 1000u;
    if (!waitForEnd(job.logPath, deadlineMs, 250, clockMs, sleepMs)) {
        std::fprintf(stderr, "[oracle] the mission's END did not come within %u s\n",
                     job.deadlineSeconds);
        endOracle(exitWhenEndDidNotCome());
    }
    std::fprintf(stderr, "[oracle] END at %.1f s\n", clockMs() / 1000.0);
    if (job.fixedStep != nullptr) {
        std::fprintf(stderr, "[clock] %llu frames, %llu spin steps, %u ms on the game's clock\n",
                     static_cast<unsigned long long>(job.fixedStep->Frames()),
                     static_cast<unsigned long long>(job.fixedStep->SpinSteps()),
                     static_cast<unsigned>(job.fixedStep->Units() /
                                           sfcop::platform::FixedStepClock::kUnitsPerMs));
        sfcop::hle::kernel32rt::Runtime &runtime = sfcop::hle::kernel32rt::Runtime::Instance();
        if (runtime.Attached()) {
            std::fprintf(stderr, "[clock] multimedia timers %s, %llu callbacks on frames\n",
                         runtime.TimerQueue().OnFrames() ? "on frames" : "on their threads",
                         static_cast<unsigned long long>(runtime.TimerQueue().FiredOnFrames()));
        }
    }
    const std::optional<std::string> exe = sha256OfFile(job.exePath);
    if (!exe) {
        std::fprintf(stderr, "[oracle] cannot hash the exe\n");
        endOracle(exitWhenExeUnreadable());
    }
    TraceHeader header;
    header.exeSha256 = *exe;
    header.build = "native " + presetName() + " " + commitOf(job.sourceDir, job.scratchDir);
    const int made = makeTrace(job.tools, job.logPath, job.outPath, job.names, header);
    if (made != 0) {
        std::fprintf(stderr, "[oracle] mklog.py refused the log (exit %d)\n", made);
        endOracle(exitAfterMklog(made));
    }
    std::fprintf(stderr, "[oracle] trace written: %s (%s)\n", fileNameOf(job.outPath).c_str(),
                 header.build.c_str());
    if (!job.expectedPath) {
        endOracle(exitAfterMklog(made));
    }
    const int verdict = compareTraces(job.tools, *job.expectedPath, job.outPath);
    std::fprintf(stderr, "[oracle] compare.py exit %d\n", verdict);
    endOracle(exitAfterCompare(verdict));
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
    options.logsDir = start.options.logsDir.value_or("");
    options.headless = start.options.headless;
    options.watchdogSeconds = start.options.watchdogSeconds;
    options.windowMode = start.options.window == WindowMode::kFullscreen
                             ? platform::WindowMode::kFullscreen
                             : platform::WindowMode::kWindowed;
    if (start.options.windowSize) {
        options.windowWidth = start.options.windowSize->width;
        options.windowHeight = start.options.windowSize->height;
    }
    std::optional<std::string> recordTrace;
    const std::vector<std::string> &extras = start.options.passThrough;
    for (std::size_t i = 0; i < extras.size(); ++i) {
        const std::string &flag = extras[i];
        options.trace = options.trace || flag == "--trace";
        options.silent = options.silent || flag == "--silent";
        options.dryRun = options.dryRun || flag == "--dry-run";
        if (flag == "--record-trace" && i + 1 < extras.size()) {
            recordTrace = extras[++i];
        } else if (flag == "--shots" && i + 1 < extras.size()) {
            options.shotsDir = extras[++i];
        } else if (flag == "--shot-every" && i + 1 < extras.size()) {
            const double seconds = std::atof(extras[++i].c_str());
            if (seconds > 0) {
                options.shotEverySeconds = seconds;
            }
        }
    }

    // The guest host makes the data folder the working directory, so every path the oracle thread
    // and the logs use is made absolute here, against the folder the client was started in.
    const std::string startedIn = std::filesystem::current_path().string();
    options.dataDir = app::oracle::absoluteFrom(startedIn, options.dataDir);
    if (!options.shotsDir.empty()) {
        options.shotsDir = app::oracle::absoluteFrom(startedIn, options.shotsDir);
    }
    if (!options.logsDir.empty()) {
        options.logsDir = app::oracle::absoluteFrom(startedIn, options.logsDir);
    }
    // The folder the game's .txt and .log files land in, and the oracle run around the game.
    const std::string logsDir =
        options.logsDir.empty() ? joinPath(options.dataDir, "logs") : options.logsDir;
    std::optional<OracleJob> oracleJob;
    if (start.options.oracleTrace || recordTrace) {
        if (!start.options.inputScript) {
            std::fprintf(stderr, "sfcop: --record-trace needs --input SCRIPT as well\n");
            return kExitBadInput;
        }
        OracleJob job;
        if (start.options.oracleTrace) {
            job.expectedPath = app::oracle::absoluteFrom(startedIn, *start.options.oracleTrace);
        }
        if (recordTrace) {
            recordTrace = app::oracle::absoluteFrom(startedIn, *recordTrace);
        }
        const std::string named = recordTrace ? *recordTrace : *job.expectedPath;
        const std::optional<app::oracle::TraceNames> names = app::oracle::namesOfTrace(named);
        if (!names) {
            std::fprintf(stderr, "sfcop: the trace %s is not <dir>/<mission>/<input>.trace\n",
                         named.c_str());
            return kExitBadInput;
        }
        job.names = *names;
        job.logPath = joinPath(logsDir, "OracleTrace.txt");
        job.outPath = recordTrace ? *recordTrace
                                  : joinPath(joinPath(logsDir, names->mission), names->input + ".trace");
        job.sourceDir = app::oracle::sourceDirOfHashList(SFCOP_HASH_LIST);
        job.tools = app::oracle::toolsOf(job.sourceDir, hostEnvironment());
        job.tools.dir = app::oracle::absoluteFrom(startedIn, job.tools.dir);
        job.exePath = joinPath(options.installDir, options.exeName);
        job.scratchDir = logsDir;
        // The oracle owns the deadline; without --watchdog it waits a day.
        job.deadlineSeconds = options.watchdogSeconds != 0 ? options.watchdogSeconds : 86400;
        options.watchdogSeconds = 0;
        // A log left by an earlier run would end this one at once.
        std::error_code error;
        std::filesystem::remove(job.logPath, error);
        oracleJob = std::move(job);
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
    // --oracle, --record-trace and --fixed-step: the game's clock advances a fixed step per frame
    // the pump counts (guest::Options::onFrame), so two runs read the same times
    // (platform/Clock/FixedStepClock.h, oracle/README.md "The fixed step"). This thread is the
    // game's main thread, the one whose sleeps count.
    const sfcop::platform::FixedStepClock *fixedStep = nullptr;
    if (const std::optional<std::uint32_t> step = app::oracle::fixedStepOf(
            start.options.fixedStepMicroseconds, oracleJob.has_value());
        step && clock) {
        auto stepped = std::make_unique<platform::FixedStepClock>(
            std::move(clock), platform::fixedStepQuantumFromMicroseconds(*step));
        stepped->BindToCallingThread();
        platform::FixedStepClock *advanced = stepped.get();
        // After the step, the multimedia timers due by it fire here, on this thread
        // (hle/kernel32rt/Timers.h: on a fixed-step clock they have no thread of their own).
        options.onFrame = [advanced] {
            advanced->AdvanceFrame();
            hle::kernel32rt::Runtime &runtime = hle::kernel32rt::Runtime::Instance();
            if (runtime.Attached()) {
                runtime.TimerQueue().FireDue();
            }
        };
        fixedStep = advanced;
        clock = std::move(stepped);
        std::fprintf(stderr, "[clock] fixed step, %s ms per frame\n",
                     app::oracle::millisecondsText(*step).c_str());
        if (oracleJob) {
            oracleJob->fixedStep = fixedStep;
        }
    }
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
        environment.fileExists = [dataDir, logsDir](const std::string &path) {
            return scriptFileExists(dataDir, logsDir, path);
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
    std::optional<std::thread> oracleThread;
    if (oracleJob) {
        std::fprintf(stderr, "[oracle] waiting for END in %s, up to %u s\n",
                     app::oracle::fileNameOf(oracleJob->logPath).c_str(),
                     oracleJob->deadlineSeconds);
        oracleThread.emplace([job = *oracleJob] { runOracle(job); });
        oracleThread->detach();
    }
    return guest::Run(options, components);
}
