// The native client's command line (DESIGN §6.8):
//
//   sfcop [--install DIR] [--data DIR] [--logs DIR] [--windowed [WxH]|--fullscreen]
//         [--fix NAME=on|off] [--oracle TRACE --input SCRIPT --headless --watchdog S]
//         [--fixed-step MS]
//   sfcop --import DIR [--data DIR]
//
// --import DIR copies the installed game in or below DIR into the client's own folder, verified
// (app/Import.h), remembers it, and exits; later runs use that copy with no --install. It cannot
// be given with --install.
// --windowed WxH opens the window W by H points with the game's frame scaled into it; without
// WxH the window takes the game's own size. --fullscreen scales the frame to the display.
// --input may be given alone: a script with no trace is a menu run. --oracle needs --input.
// --fixed-step MS runs the game's clock at MS milliseconds per frame instead of the wall clock
// (platform/Clock/FixedStepClock.h); --oracle and --record-trace imply it at the default step.
// --logs DIR names the folder where the files the game writes beside its exe land (its .txt and
// .log files); without it they go to logs/ under the data folder (src/hle/kernel/README.md).
//
// One table in CommandLine.cpp holds every flag; parsing and `--help` both read it. A value is
// given as `--flag VALUE` or `--flag=VALUE`; --windowed's WxH is optional. Parsing checks form
// only: whether a folder exists or a fix is known is for the code that uses the value.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sfcop::app {

enum class WindowMode { kDefault, kWindowed, kFullscreen };

// A window size in points, each side 1 to 16384.
struct WindowSize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct FixSetting {
    std::string name;
    bool on = false;
};

struct ClientOptions {
    std::optional<std::string> installDir;
    // --import DIR: import the game from DIR and exit.
    std::optional<std::string> importFrom;
    std::optional<std::string> dataDir;
    std::optional<std::string> logsDir;
    WindowMode window = WindowMode::kDefault;
    // --windowed WxH; absent: the window takes the game's size.
    std::optional<WindowSize> windowSize;
    // In command-line order, each name at most once.
    std::vector<FixSetting> fixes;
    std::optional<std::string> oracleTrace;
    std::optional<std::string> inputScript;
    bool headless = false;
    // 0 means no watchdog, as when the flag is absent.
    unsigned watchdogSeconds = 0;
    // --fixed-step: the frame in whole microseconds (1 to 1,000,000; MS has at most three
    // decimals).
    std::optional<std::uint32_t> fixedStepMicroseconds;
    // The caller's extra flags (see ExtraFlag), verbatim and in order: "--name" and, for a flag
    // with a value, the value as the next element.
    std::vector<std::string> passThrough;
};

// A flag the caller adds to the table, such as the temporary boot platform's own. It is listed
// by `--help` and passed through unchecked. `value` names its value ("DIR"), empty for none.
struct ExtraFlag {
    std::string_view name;
    std::string_view value;
    std::string_view help;
};

enum class ParseOutcome { kRun, kHelp, kError };

struct ParseResult {
    ParseOutcome outcome = ParseOutcome::kRun;
    ClientOptions options;
    // For kError: one sentence that names the flag or argument at fault.
    std::string error;

    // 0 to run or after --help, 2 (bad input) on an error.
    int exitCode() const;
};

// `args` excludes the program name.
ParseResult parseCommandLine(const std::vector<std::string> &args,
                             const std::vector<ExtraFlag> &extras = {});
ParseResult parseCommandLine(int argc, const char *const *argv,
                             const std::vector<ExtraFlag> &extras = {});

// The text `--help` prints, generated from the flag table and the exit codes.
std::string helpText(const std::vector<ExtraFlag> &extras = {});

// Whether the guest host also reads the window inside the game's calls during its loads
// (guest::Options::pumpDuringLoads). Not for a run whose frames a steps file or a trace counts
// (--input, --oracle, or the client's --record-trace among the extra flags) or that has no window
// (--headless): those read the window only when the game peeks. --fixed-step alone is a clock
// mode and leaves it on.
bool readsWindowDuringLoads(const ClientOptions &options);

} // namespace sfcop::app
