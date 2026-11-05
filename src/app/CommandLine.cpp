#include "app/CommandLine.h"

#include <algorithm>
#include <cstdio>
#include <set>

#include "app/ExitCodes.h"

namespace sfcop::app {

namespace {

constexpr unsigned kMaxWatchdogSeconds = 86400;

// Applies a flag's value to the options; returns false with `error` set when the value is
// malformed. `value` is empty for a flag that takes none.
using ApplyFn = bool (*)(ClientOptions &options, const std::string &value, std::string &error);

struct Flag {
    std::string_view name;
    // What the value is called in the help, empty for a flag without one.
    std::string_view value;
    std::string_view help;
    ApplyFn apply;
};

bool isFixName(const std::string &name) {
    if (name.empty() || name[0] < 'a' || name[0] > 'z') {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

bool applyWindowMode(ClientOptions &options, WindowMode mode, std::string &error) {
    if (options.window != WindowMode::kDefault && options.window != mode) {
        error = "--windowed and --fullscreen cannot be given together";
        return false;
    }
    options.window = mode;
    return true;
}

// The flags of DESIGN §6.8, in the order `--help` lists them.
const Flag kFlags[] = {
    {"--install", "DIR", "the install folder (default: the one remembered, else the usual places)",
     [](ClientOptions &o, const std::string &v, std::string &) {
         o.installDir = v;
         return true;
     }},
    {"--data", "DIR", "the per-user data folder (default: the platform's own)",
     [](ClientOptions &o, const std::string &v, std::string &) {
         o.dataDir = v;
         return true;
     }},
    {"--windowed", "", "play in a window",
     [](ClientOptions &o, const std::string &, std::string &e) {
         return applyWindowMode(o, WindowMode::kWindowed, e);
     }},
    {"--fullscreen", "", "play full screen",
     [](ClientOptions &o, const std::string &, std::string &e) {
         return applyWindowMode(o, WindowMode::kFullscreen, e);
     }},
    {"--fix", "NAME=on|off", "turn one fix on or off; repeat for more fixes",
     [](ClientOptions &o, const std::string &v, std::string &e) {
         const std::size_t equals = v.find('=');
         const std::string name = v.substr(0, equals);
         const std::string state = equals == std::string::npos ? "" : v.substr(equals + 1);
         if (!isFixName(name) || (state != "on" && state != "off")) {
             e = "--fix takes NAME=on or NAME=off, with NAME in lower_snake_case, not '" + v + "'";
             return false;
         }
         for (const FixSetting &fix : o.fixes) {
             if (fix.name == name) {
                 e = "--fix " + name + " is given twice";
                 return false;
             }
         }
         o.fixes.push_back({name, state == "on"});
         return true;
     }},
    {"--oracle", "TRACE", "run the replay oracle and compare with TRACE; needs --input",
     [](ClientOptions &o, const std::string &v, std::string &) {
         o.oracleTrace = v;
         return true;
     }},
    {"--input", "SCRIPT", "play the steps file SCRIPT into the game (--oracle needs it)",
     [](ClientOptions &o, const std::string &v, std::string &) {
         o.inputScript = v;
         return true;
     }},
    {"--headless", "", "no window and no sound",
     [](ClientOptions &o, const std::string &, std::string &) {
         o.headless = true;
         return true;
     }},
    {"--watchdog", "S", "stop after S seconds, 0 to 86400 (0, the default: never)",
     [](ClientOptions &o, const std::string &v, std::string &e) {
         unsigned long seconds = 0;
         bool ok = !v.empty() && v.size() <= 5;
         for (char c : v) {
             ok = ok && c >= '0' && c <= '9';
             seconds = seconds * 10 + static_cast<unsigned long>(c - '0');
         }
         if (!ok || seconds > kMaxWatchdogSeconds) {
             e = "--watchdog takes a whole number of seconds from 0 to 86400, not '" + v + "'";
             return false;
         }
         o.watchdogSeconds = static_cast<unsigned>(seconds);
         return true;
     }},
};

constexpr std::string_view kHelpName = "--help";
constexpr std::string_view kHelpShort = "-h";
constexpr std::string_view kHelpHelp = "print this help and exit";

const Flag *findFlag(std::string_view name) {
    for (const Flag &flag : kFlags) {
        if (flag.name == name) {
            return &flag;
        }
    }
    return nullptr;
}

const ExtraFlag *findExtra(const std::vector<ExtraFlag> &extras, std::string_view name) {
    for (const ExtraFlag &flag : extras) {
        if (flag.name == name) {
            return &flag;
        }
    }
    return nullptr;
}

ParseResult failWith(std::string error) {
    ParseResult result;
    result.outcome = ParseOutcome::kError;
    result.error = std::move(error);
    return result;
}

void appendHelpLine(std::string &out, std::string_view name, std::string_view value,
                    std::string_view help) {
    constexpr std::size_t kColumn = 24;
    std::string left = "  " + std::string(name);
    if (!value.empty()) {
        left += " " + std::string(value);
    }
    out += left;
    if (left.size() + 2 > kColumn) {
        out += "\n" + std::string(kColumn, ' ');
    } else {
        out += std::string(kColumn - left.size(), ' ');
    }
    out += std::string(help) + "\n";
}

} // namespace

int ParseResult::exitCode() const {
    return outcome == ParseOutcome::kError ? kExitBadInput : kExitSuccess;
}

ParseResult parseCommandLine(const std::vector<std::string> &args,
                             const std::vector<ExtraFlag> &extras) {
    ParseResult result;
    std::set<std::string_view> seen;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string &arg = args[i];
        if (arg == kHelpName || arg == kHelpShort) {
            result.outcome = ParseOutcome::kHelp;
            return result;
        }
        if (arg.rfind("--", 0) != 0) {
            return failWith("unexpected argument '" + arg + "'");
        }
        const std::size_t equals = arg.find('=');
        const std::string name = arg.substr(0, equals);
        const bool inlineValue = equals != std::string::npos;

        const Flag *flag = findFlag(name);
        const ExtraFlag *extra = flag == nullptr ? findExtra(extras, name) : nullptr;
        if (flag == nullptr && extra == nullptr) {
            return failWith("unknown flag '" + name + "'");
        }
        const std::string_view valueName = flag != nullptr ? flag->value : extra->value;

        std::string value;
        if (valueName.empty()) {
            if (inlineValue) {
                return failWith(name + " takes no value");
            }
        } else if (inlineValue) {
            value = arg.substr(equals + 1);
        } else if (i + 1 < args.size() && args[i + 1].rfind("--", 0) != 0) {
            value = args[++i];
        } else {
            return failWith(name + " needs " + std::string(valueName));
        }
        if (!valueName.empty() && value.empty()) {
            return failWith(name + " needs " + std::string(valueName) + ", not an empty value");
        }

        if (extra != nullptr) {
            result.options.passThrough.push_back(name);
            if (!valueName.empty()) {
                result.options.passThrough.push_back(value);
            }
            continue;
        }
        // --fix may be repeated (it checks its names itself); every other flag is given once.
        if (flag->name != "--fix" && !seen.insert(flag->name).second) {
            return failWith(name + " is given twice");
        }
        std::string error;
        if (!flag->apply(result.options, value, error)) {
            return failWith(error);
        }
    }

    const ClientOptions &options = result.options;
    if (options.oracleTrace && !options.inputScript) {
        return failWith("--oracle needs --input SCRIPT as well");
    }
    return result;
}

ParseResult parseCommandLine(int argc, const char *const *argv,
                             const std::vector<ExtraFlag> &extras) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return parseCommandLine(args, extras);
}

std::string helpText(const std::vector<ExtraFlag> &extras) {
    std::string out = "usage: sfcop [options]\n\noptions:\n";
    for (const Flag &flag : kFlags) {
        appendHelpLine(out, flag.name, flag.value, flag.help);
    }
    appendHelpLine(out, std::string(kHelpName) + ", " + std::string(kHelpShort), "", kHelpHelp);
    if (!extras.empty()) {
        out += "\nother options:\n";
        for (const ExtraFlag &flag : extras) {
            appendHelpLine(out, flag.name, flag.value, flag.help);
        }
    }
    out += "\nexit codes:\n";
    for (const ExitCodeText &code : kExitCodeTexts) {
        char line[16];
        std::snprintf(line, sizeof line, "  %d  ", static_cast<int>(code.code));
        out += line;
        out += code.meaning;
        out += "\n";
    }
    return out;
}

} // namespace sfcop::app
