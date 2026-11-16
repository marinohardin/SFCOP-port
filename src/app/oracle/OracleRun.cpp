#include "app/oracle/OracleRun.h"

#include "app/ExitCodes.h"
#include "app/Sha256.h"
#include "platform/Clock/FixedStepClock.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace sfcop::app::oracle {

namespace {

namespace fs = std::filesystem;

std::string lastLine(const std::string &text) {
    std::string body = text;
    while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) {
        body.pop_back();
    }
    const std::size_t cut = body.find_last_of('\n');
    std::string line = cut == std::string::npos ? body : body.substr(cut + 1);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }
    return line;
}

} // namespace

std::optional<TraceNames> namesOfTrace(const std::string &tracePath) {
    const fs::path path(tracePath);
    if (path.extension() != ".trace" || path.stem().empty() || !path.has_parent_path()) {
        return std::nullopt;
    }
    const std::string mission = path.parent_path().filename().string();
    if (mission.empty() || mission == "." || mission == "..") {
        return std::nullopt;
    }
    return TraceNames{mission, path.stem().string()};
}

bool logHasEnd(const std::string &logText) {
    if (logText.empty() || logText.back() != '\n') {
        return false; // the recorder's last write ends in LF; anything else is still coming
    }
    return lastLine(logText).rfind("END ", 0) == 0;
}

std::optional<std::string> readText(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

bool waitForEnd(const std::string &logPath, std::uint32_t deadlineMs, std::uint32_t pollMs,
                const std::function<std::uint32_t()> &clockMs,
                const std::function<void(std::uint32_t)> &sleepMs) {
    const std::uint32_t start = clockMs();
    for (;;) {
        const std::optional<std::string> text = readText(logPath);
        if (text && logHasEnd(*text)) {
            return true;
        }
        if (clockMs() - start >= deadlineMs) {
            return false;
        }
        sleepMs(pollMs);
    }
}

Tools toolsOf(const std::string &sourceDir,
              const std::function<std::optional<std::string>(const std::string &)> &env) {
    Tools tools;
    const std::optional<std::string> dir = env ? env("SFCOP_ORACLE_TOOLS") : std::nullopt;
    // Joined with '/' by hand: std::filesystem would use '\' on Windows, and the folder is a
    // shell argument on every host.
    tools.dir = dir && !dir->empty() ? *dir : sourceDir + "/oracle/trace";
    const std::optional<std::string> python = env ? env("SFCOP_PYTHON") : std::nullopt;
    tools.python = python && !python->empty() ? *python : "python3";
    return tools;
}

std::string quoteArgument(const std::string &argument) {
#if defined(_WIN32)
    std::string out = "\"";
    for (char c : argument) {
        if (c == '"') {
            out += '\\';
        }
        out += c;
    }
    return out + "\"";
#else
    std::string out = "'";
    for (char c : argument) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    return out + "'";
#endif
}

int runCommand(const std::string &commandLine) {
    const int status = std::system(commandLine.c_str());
    if (status == -1) {
        return 255;
    }
#if defined(_WIN32)
    return status;
#else
    // POSIX: the wait status. A signal is reported as 128 plus its number, as shells do.
    if ((status & 0x7F) != 0) {
        return 128 + (status & 0x7F);
    }
    return (status >> 8) & 0xFF;
#endif
}

int makeTrace(const Tools &tools, const std::string &logPath, const std::string &outPath,
              const TraceNames &names, const TraceHeader &header) {
    std::error_code ignored;
    fs::create_directories(fs::path(outPath).parent_path(), ignored);
    const std::string command =
        quoteArgument(tools.python) + " -I " +
        quoteArgument((fs::path(tools.dir) / "mklog.py").string()) + " " +
        quoteArgument(logPath) + " --mission " + quoteArgument(names.mission) + " --input " +
        quoteArgument(names.input) + " --exe-sha256 " + quoteArgument(header.exeSha256) +
        " --build " + quoteArgument(header.build) + " -o " + quoteArgument(outPath);
    return runCommand(command);
}

int compareTraces(const Tools &tools, const std::string &expectedPath,
                  const std::string &actualPath) {
    const std::string command = quoteArgument(tools.python) + " -I " +
                                quoteArgument((fs::path(tools.dir) / "compare.py").string()) +
                                " " + quoteArgument(expectedPath) + " " +
                                quoteArgument(actualPath);
    return runCommand(command);
}

int exitWhenEndDidNotCome() { return kExitFailure; }

int exitWhenExeUnreadable() { return kExitFailure; }

int exitAfterMklog(int mklogStatus) {
    if (mklogStatus == 0) {
        return kExitSuccess;
    }
    return mklogStatus == 2 ? kExitBadInput : kExitFailure;
}

int exitAfterCompare(int compareStatus) {
    switch (compareStatus) {
    case 0:
        return kExitSuccess;
    case 4:
        return kExitOracleMismatch;
    case 2:
        return kExitBadInput;
    default:
        return kExitFailure;
    }
}

std::string absoluteFrom(const std::string &base, const std::string &path) {
    const fs::path given(path);
    // Rooted, not is_absolute(): "/x" is rooted on every host, "C:\\x" on Windows too.
    if (given.has_root_directory()) {
        return path;
    }
    // Slashes on every host, so the result reads the same in a log and in a test.
    return (fs::path(base) / given).lexically_normal().generic_string();
}

std::string fileNameOf(const std::string &path) { return fs::path(path).filename().string(); }

std::string presetName() {
#if defined(_WIN32)
    return "windows-x64";
#elif defined(__APPLE__)
    return "macos-arm64";
#else
    return "linux-x64";
#endif
}

std::string commitOf(const std::string &sourceDir, const std::string &scratchDir) {
    std::error_code ignored;
    fs::create_directories(scratchDir, ignored);
    const std::string out = (fs::path(scratchDir) / "commit.txt").string();
#if defined(_WIN32)
    const char *quiet = " 2>NUL";
#else
    const char *quiet = " 2>/dev/null";
#endif
    const std::string command = "git -C " + quoteArgument(sourceDir) +
                                " rev-parse --short=12 HEAD > " + quoteArgument(out) + quiet;
    std::string commit = "unknown";
    if (runCommand(command) == 0) {
        const std::optional<std::string> text = readText(out);
        if (text) {
            const std::string line = lastLine(*text);
            if (!line.empty()) {
                commit = line;
            }
        }
    }
    fs::remove(out, ignored);
    return commit;
}

std::optional<std::string> sha256OfFile(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    Sha256 hash;
    char chunk[65536];
    while (in.read(chunk, sizeof chunk) || in.gcount() > 0) {
        hash.update(chunk, static_cast<std::size_t>(in.gcount()));
    }
    return Sha256::toHex(hash.finish());
}

std::optional<std::uint32_t> fixedStepOf(std::optional<std::uint32_t> flagMicroseconds,
                                         bool oracleRun) {
    if (flagMicroseconds) {
        return flagMicroseconds;
    }
    if (oracleRun) {
        return platform::kDefaultFixedStepMicroseconds;
    }
    return std::nullopt;
}

std::string millisecondsText(std::uint32_t microseconds) {
    char text[24];
    std::snprintf(text, sizeof text, "%u.%03u", microseconds / 1000, microseconds % 1000);
    return text;
}

} // namespace sfcop::app::oracle
