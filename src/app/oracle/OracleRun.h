// The replay oracle run (DESIGN §5.10, §6.5, §6.8): what `sfcop --oracle TRACE --input SCRIPT`
// does around the game.
//
// The input script sets the oracle mission up through the game's own menus and starts it; the
// mission writes its log, OracleTrace.txt, beside the exe, which the kernel module puts in the
// logs folder (--logs DIR, else logs/ under the data folder). This code waits for the log's END
// record, turns the log into a trace with oracle/trace/mklog.py, compares it with TRACE using
// oracle/trace/compare.py, and the client exits with the comparator's status: 0 identical, 4 a
// tick differs, 2 a malformed log or trace. A run whose END never comes ends with 1 at its
// deadline (the --watchdog, which the oracle owns on such a run).
//
// Names. A trace path is `<root>/<mission>/<input>.trace` (oracle/expected/README.md): the
// mission is the parent folder's name and the input the file's stem. The actual trace of a run
// is written at `<logs>/<mission>/<input>.trace`, so `compare.py --tree EXPECTED_ROOT <logs>`
// finds it (scripts/check.sh). `--record-trace OUT` plays and writes OUT without comparing,
// taking its names from OUT's path the same way.
//
// The trace tools are oracle/trace/ of the source tree the client was built from, which the
// build config names; SFCOP_ORACLE_TOOLS overrides it. They are run through python3
// (SFCOP_PYTHON overrides the interpreter) with std::system, so this file holds no platform code.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace sfcop::app::oracle {

struct TraceNames {
    std::string mission;
    std::string input;
};

// `<root>/<mission>/<input>.trace`; nothing when the path has no such two components.
std::optional<TraceNames> namesOfTrace(const std::string &tracePath);

// The log's last line is an END record, so the recorder has closed it.
bool logHasEnd(const std::string &logText);

// Reads a file whole; nothing when it cannot be read.
std::optional<std::string> readText(const std::string &path);

// Polls `logPath` every `pollMs` until logHasEnd() or `deadlineMs` of `clockMs` have passed.
// True when END came.
bool waitForEnd(const std::string &logPath, std::uint32_t deadlineMs, std::uint32_t pollMs,
                const std::function<std::uint32_t()> &clockMs,
                const std::function<void(std::uint32_t)> &sleepMs);

// Where the trace tools are and how they are run.
struct Tools {
    std::string dir;    // the folder holding mklog.py and compare.py
    std::string python; // the interpreter
};

// oracle/trace under the source tree, or $SFCOP_ORACLE_TOOLS; python3, or $SFCOP_PYTHON.
Tools toolsOf(const std::string &sourceDir,
              const std::function<std::optional<std::string>(const std::string &)> &env);

// One argument as the platform's shell takes it.
std::string quoteArgument(const std::string &argument);
// Runs a command line through the shell; its exit status (255 when it could not be run).
int runCommand(const std::string &commandLine);

struct TraceHeader {
    std::string exeSha256;
    std::string build; // "native <preset> <commit>"
};

// `python3 mklog.py LOG --mission --input --exe-sha256 --build -o OUT`; mklog's status.
int makeTrace(const Tools &tools, const std::string &logPath, const std::string &outPath,
              const TraceNames &names, const TraceHeader &header);
// `python3 compare.py EXPECTED ACTUAL`; compare's status (0 identical, 4 a tick differs, 2 bad).
int compareTraces(const Tools &tools, const std::string &expectedPath,
                  const std::string &actualPath);

// What an oracle run ends with (DESIGN §6.8, ExitCodes.h): the mission's END did not come before
// the deadline, the exe could not be hashed, or a trace tool ran. `mklog.py` gives 0 or 2 (a log
// the format refuses); `compare.py` gives 0 identical, 4 a tick differs, 2 a malformed or
// mismatched trace; any other status (the tool could not run, a signal) is a failure of the run.
int exitWhenEndDidNotCome();
int exitWhenExeUnreadable();
int exitAfterMklog(int mklogStatus);
int exitAfterCompare(int compareStatus);

// `path` made absolute against `base`, '.' and '..' resolved lexically; unchanged when it is
// absolute already. The guest host makes the data folder the process's working directory, so every
// path the oracle thread uses is made absolute before the game starts.
std::string absoluteFrom(const std::string &base, const std::string &path);

// A path's file name alone: the oracle's log lines never print a folder, which without --logs
// is under the data folder.
std::string fileNameOf(const std::string &path);

// The preset this client was built for: macos-arm64, windows-x64 or linux-x64.
std::string presetName();
// The short commit of the source tree (`git rev-parse --short=12 HEAD`, through a temporary file
// under `scratchDir`), or "unknown".
std::string commitOf(const std::string &sourceDir, const std::string &scratchDir);
// SHA-256 of a file, 64 lowercase hex digits; nothing when it cannot be read.
std::optional<std::string> sha256OfFile(const std::string &path);

// The fixed step a run's clock takes, in microseconds per frame (README.md, "The fixed step"):
// --fixed-step's value when given; else the default step for an oracle run (--oracle or
// --record-trace); else none, and the run keeps the host clock.
std::optional<std::uint32_t> fixedStepOf(std::optional<std::uint32_t> flagMicroseconds,
                                         bool oracleRun);
// "16.667": microseconds as milliseconds with three decimals, for the log line.
std::string millisecondsText(std::uint32_t microseconds);

} // namespace sfcop::app::oracle
