// The native client's command line (DESIGN §6.8):
//
//   sfcop [--install DIR] [--data DIR] [--windowed|--fullscreen] [--fix NAME=on|off]
//         [--oracle TRACE --input SCRIPT --headless --watchdog S]
//
// --input may be given alone: a script with no trace is a menu run. --oracle needs --input.
//
// One table in CommandLine.cpp holds every flag; parsing and `--help` both read it. A value is
// given as `--flag VALUE` or `--flag=VALUE`. Parsing checks form only: whether a folder exists
// or a fix is known is for the code that uses the value.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sfcop::app {

enum class WindowMode { kDefault, kWindowed, kFullscreen };

struct FixSetting {
    std::string name;
    bool on = false;
};

struct ClientOptions {
    std::optional<std::string> installDir;
    std::optional<std::string> dataDir;
    WindowMode window = WindowMode::kDefault;
    // In command-line order, each name at most once.
    std::vector<FixSetting> fixes;
    std::optional<std::string> oracleTrace;
    std::optional<std::string> inputScript;
    bool headless = false;
    // 0 means no watchdog, as when the flag is absent.
    unsigned watchdogSeconds = 0;
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

} // namespace sfcop::app
