// The oracle run around the game (OracleRun.h): the names, the END wait, and the exit codes the
// trace tools give on C5's fixtures (oracle/trace/fixtures/, invented).

#include "app/oracle/OracleRun.h"

#include "app/FilesMock.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace sfcop::app::oracle {
namespace {

namespace fs = std::filesystem;

// tests/fixtures is <source>/tests/fixtures (FilesMock.h).
std::string sourceDir() { return fs::path(testing::fixturesDir()).parent_path().parent_path().string(); }
std::string traceFixtures() { return sourceDir() + "/oracle/trace/fixtures"; }

Tools tools() {
    return toolsOf(sourceDir(), [](const std::string &name) -> std::optional<std::string> {
        const char *value = std::getenv(name.c_str());
        return value != nullptr ? std::optional<std::string>(value) : std::nullopt;
    });
}

bool havePython() {
    static const bool have = runCommand(quoteArgument(tools().python) + " -c pass") == 0;
    return have;
}

struct Scratch {
    fs::path dir;
    Scratch() {
        dir = fs::temp_directory_path() /
              ("sfcop-oracle-test-" + std::to_string(std::random_device{}()));
        fs::create_directories(dir);
    }
    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(dir, ignored);
    }
    std::string write(const char *name, const std::string &text) const {
        const fs::path path = dir / name;
        std::ofstream(path, std::ios::binary) << text;
        return path.string();
    }
};

const char kExe[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

} // namespace

TEST_CASE("a trace path names its mission and input by its last two components") {
    const std::optional<TraceNames> names = namesOfTrace("oracle/expected/OracleSkirmish/skirmish.trace");
    REQUIRE(names.has_value());
    CHECK(names->mission == "OracleSkirmish");
    CHECK(names->input == "skirmish");
    CHECK(namesOfTrace("/abs/root/Mission_2/menu-run.trace")->mission == "Mission_2");
    CHECK_FALSE(namesOfTrace("skirmish.trace").has_value());
    CHECK_FALSE(namesOfTrace("OracleSkirmish/skirmish.txt").has_value());
    CHECK_FALSE(namesOfTrace("OracleSkirmish/.trace").has_value());
}

TEST_CASE("a log has ended only when its last line is an END record with its LF") {
    CHECK(logHasEnd("# oracle-trace v1\nS 0 c101 ...\nEND 1 max_samples\n"));
    CHECK(logHasEnd("# oracle-trace v1\r\nEND 0 aborted\r\n"));
    CHECK_FALSE(logHasEnd("# oracle-trace v1\nEND 1 max_samples")); // still being written
    CHECK_FALSE(logHasEnd("# oracle-trace v1\nS 0 c101 ...\n"));
    CHECK_FALSE(logHasEnd(""));
    CHECK_FALSE(logHasEnd("ENDING 1 x\n"));
}

TEST_CASE("waitForEnd returns once END is in the file and false at its deadline") {
    Scratch scratch;
    const std::string log = (scratch.dir / "OracleTrace.txt").string();
    std::uint32_t now = 0;
    const auto clock = [&now] { return now; };
    const auto sleep = [&now, &scratch, log](std::uint32_t ms) {
        now += ms;
        if (now == 750) {
            scratch.write("OracleTrace.txt", "# oracle-trace v1\nEND 2 one_side_gone\n");
        }
    };
    CHECK(waitForEnd(log, 5000, 250, clock, sleep));
    CHECK(now == 750);

    now = 0;
    const std::string missing = (scratch.dir / "none.txt").string();
    const auto plainSleep = [&now](std::uint32_t ms) { now += ms; };
    CHECK_FALSE(waitForEnd(missing, 1000, 250, clock, plainSleep));
    CHECK(now == 1000);
}

TEST_CASE("the tools are oracle/trace under the source tree unless the environment says otherwise") {
    CHECK(sourceDirOfHashList("/src/tree/analysis/results/retail_hashes.sha256") == "/src/tree");
    const Tools plain = toolsOf("/src/tree", [](const std::string &) { return std::nullopt; });
    CHECK(plain.dir == "/src/tree/oracle/trace");
    CHECK(plain.python == "python3");
    const Tools set = toolsOf("/src/tree", [](const std::string &name) -> std::optional<std::string> {
        if (name == "SFCOP_ORACLE_TOOLS") {
            return "/elsewhere/trace";
        }
        if (name == "SFCOP_PYTHON") {
            return "/opt/python3.12";
        }
        return std::nullopt;
    });
    CHECK(set.dir == "/elsewhere/trace");
    CHECK(set.python == "/opt/python3.12");
}

TEST_CASE("an argument is quoted for the shell") {
#if defined(_WIN32)
    CHECK(quoteArgument("a b") == "\"a b\"");
#else
    CHECK(quoteArgument("a b") == "'a b'");
    CHECK(quoteArgument("it's") == "'it'\\''s'");
#endif
    const std::string preset = presetName();
    CHECK((preset == "macos-arm64" || preset == "windows-x64" || preset == "linux-x64"));
}

TEST_CASE("the mission log fixture becomes the hand-hashed trace, and compares identical" *
          doctest::skip(!havePython())) {
    Scratch scratch;
    const std::string out = (scratch.dir / "OracleSkirmish" / "skirmish.trace").string();
    const int made = makeTrace(tools(), traceFixtures() + "/oracle_log.txt", out,
                               TraceNames{"OracleSkirmish", "skirmish"},
                               TraceHeader{kExe, "native macos-arm64 0123456789ab"});
    CHECK(made == 0);
    REQUIRE(fs::exists(out));
    // The header's build and date differ from the fixture's by design; every tick must agree.
    CHECK(compareTraces(tools(), traceFixtures() + "/oracle_log.trace", out) == 0);
}

TEST_CASE("a log the format refuses gives mklog's exit 2" * doctest::skip(!havePython())) {
    Scratch scratch;
    const std::string log = scratch.write("OracleTrace.txt", "# oracle-trace v1\nS 0 c101\n");
    const std::string out = (scratch.dir / "M" / "i.trace").string();
    CHECK(makeTrace(tools(), log, out, TraceNames{"M", "i"}, TraceHeader{kExe, "native x y"}) == 2);
    CHECK_FALSE(fs::exists(out));
}

TEST_CASE("comparing gives 0 on a match, 4 on a tick-3 difference, 2 on a bad trace" *
          doctest::skip(!havePython())) {
    const std::string expected = traceFixtures() + "/expected.trace";
    CHECK(compareTraces(tools(), expected, traceFixtures() + "/actual_same.trace") == 0);
    CHECK(compareTraces(tools(), expected, traceFixtures() + "/actual_tick3.trace") == 4);
    CHECK(compareTraces(tools(), expected, traceFixtures() + "/actual_truncated.trace") == 4);
    CHECK(compareTraces(tools(), expected, traceFixtures() + "/actual_header_input.trace") == 2);
    CHECK(compareTraces(tools(), expected, traceFixtures() + "/no_such.trace") == 2);
}

TEST_CASE("the oracle run's exit codes follow DESIGN 6.8 for every tool status") {
    // compare.py: 0 identical, 4 a tick differs, 2 a malformed or mismatched trace; anything else
    // (the tool could not run: 127 or 255, a signal: 128 + n) is a failure of the run, 1.
    CHECK(exitAfterCompare(0) == 0);
    CHECK(exitAfterCompare(4) == 4);
    CHECK(exitAfterCompare(2) == 2);
    CHECK(exitAfterCompare(1) == 1);
    CHECK(exitAfterCompare(127) == 1);
    CHECK(exitAfterCompare(255) == 1);
    CHECK(exitAfterCompare(139) == 1);
    // mklog.py: 0 written, 2 a log the format refuses; anything else is a failure of the run.
    CHECK(exitAfterMklog(0) == 0);
    CHECK(exitAfterMklog(2) == 2);
    CHECK(exitAfterMklog(1) == 1);
    CHECK(exitAfterMklog(127) == 1);
    // The deadline and the exe: the run failed to play, 1, whatever the tools would have said.
    CHECK(exitWhenEndDidNotCome() == 1);
    CHECK(exitWhenExeUnreadable() == 1);
}

TEST_CASE("an END that does not come by the deadline ends the run with 1, one that comes does not") {
    Scratch scratch;
    const std::string log = (scratch.dir / "OracleTrace.txt").string();
    std::uint32_t now = 0;
    const auto clock = [&now] { return now; };
    const auto sleep = [&now](std::uint32_t ms) { now += ms; };
    // The mission never writes END: waitForEnd gives up at the deadline, which is the verdict 1.
    const bool came = waitForEnd(log, 3000, 250, clock, sleep);
    CHECK_FALSE(came);
    CHECK(now == 3000);
    CHECK(exitWhenEndDidNotCome() == 1);
    // A log that ends in "aborted" still came to END; the trace tools judge it (mklog accepts it).
    scratch.write("OracleTrace.txt", "# oracle-trace v1\nEND 3 aborted\n");
    now = 0;
    CHECK(waitForEnd(log, 3000, 250, clock, sleep));
    CHECK(now == 0);
}

TEST_CASE("a relative path is taken from the folder the client started in, an absolute one is kept") {
    CHECK(absoluteFrom("/work/repo", "oracle/expected/OracleSkirmish/skirmish.trace") ==
          "/work/repo/oracle/expected/OracleSkirmish/skirmish.trace");
    CHECK(absoluteFrom("/work/repo", "./a/../b/c.trace") == "/work/repo/b/c.trace");
    CHECK(absoluteFrom("/work/repo", "/elsewhere/logs") == "/elsewhere/logs");
}

TEST_CASE("the oracle's log lines name a file, never a folder") {
    CHECK(fileNameOf("/home/testcaptain/data/logs/OracleTrace.txt") == "OracleTrace.txt");
    CHECK(fileNameOf("/home/testcaptain/data/logs/OracleSkirmish/skirmish.trace") ==
          "skirmish.trace");
    CHECK(fileNameOf("OracleTrace.txt") == "OracleTrace.txt");
    CHECK(fileNameOf("/home/testcaptain/data/logs/").empty());
}

TEST_CASE("the exe hash is sha256sum's and the commit comes from git or reads unknown") {
    Scratch scratch;
    const std::string empty = scratch.write("empty", "");
    CHECK(*sha256OfFile(empty) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(*sha256OfFile(scratch.write("abc", "abc")) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK_FALSE(sha256OfFile((scratch.dir / "none").string()).has_value());
    CHECK(commitOf(scratch.dir.string(), (scratch.dir / "scratch").string()) == "unknown");
    CHECK_FALSE(fs::exists(scratch.dir / "scratch" / "commit.txt"));
}

} // namespace sfcop::app::oracle
