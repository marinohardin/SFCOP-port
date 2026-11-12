#include "app/CommandLine.h"

#include <doctest/doctest.h>

using sfcop::app::ExtraFlag;
using sfcop::app::parseCommandLine;
using sfcop::app::ParseOutcome;
using sfcop::app::ParseResult;
using sfcop::app::readsWindowDuringLoads;
using sfcop::app::WindowMode;

namespace {

ParseResult parse(std::vector<std::string> args) { return parseCommandLine(args); }

void requireError(const std::vector<std::string> &args, const std::string &expected) {
    const ParseResult result = parseCommandLine(args);
    CHECK(result.outcome == ParseOutcome::kError);
    CHECK(result.exitCode() == 2);
    CHECK(result.error == expected);
}

} // namespace

TEST_CASE("an empty command line runs with every default") {
    const ParseResult result = parse({});
    REQUIRE(result.outcome == ParseOutcome::kRun);
    CHECK(result.exitCode() == 0);
    CHECK_FALSE(result.options.installDir.has_value());
    CHECK_FALSE(result.options.dataDir.has_value());
    CHECK_FALSE(result.options.logsDir.has_value());
    CHECK(result.options.window == WindowMode::kDefault);
    CHECK(result.options.fixes.empty());
    CHECK_FALSE(result.options.oracleTrace.has_value());
    CHECK_FALSE(result.options.inputScript.has_value());
    CHECK_FALSE(result.options.headless);
    CHECK(result.options.watchdogSeconds == 0);
    CHECK(result.options.passThrough.empty());
}

TEST_CASE("--install takes a folder, separated by a space or an equals sign") {
    CHECK(*parse({"--install", "/games/Orion Pirates"}).options.installDir ==
          "/games/Orion Pirates");
    CHECK(*parse({"--install=/games/op"}).options.installDir == "/games/op");
    requireError({"--install"}, "--install needs DIR");
    requireError({"--install", "--headless"}, "--install needs DIR");
    requireError({"--install="}, "--install needs DIR, not an empty value");
    requireError({"--install", "/a", "--install", "/b"}, "--install is given twice");
}

TEST_CASE("--data takes a folder") {
    CHECK(*parse({"--data", "/home/testcaptain/sfcop"}).options.dataDir ==
          "/home/testcaptain/sfcop");
    requireError({"--data"}, "--data needs DIR");
}

TEST_CASE("--logs takes a folder, given once") {
    CHECK(*parse({"--logs", "/tmp/oracle-run"}).options.logsDir == "/tmp/oracle-run");
    CHECK(*parse({"--logs=/tmp/oracle-run"}).options.logsDir == "/tmp/oracle-run");
    CHECK_FALSE(parse({"--data", "/d"}).options.logsDir.has_value());
    requireError({"--logs"}, "--logs needs DIR");
    requireError({"--logs", "/a", "--logs", "/b"}, "--logs is given twice");
}

TEST_CASE("--windowed and --fullscreen choose the window mode and exclude each other") {
    CHECK(parse({"--windowed"}).options.window == WindowMode::kWindowed);
    CHECK(parse({"--fullscreen"}).options.window == WindowMode::kFullscreen);
    requireError({"--windowed", "--fullscreen"},
                 "--windowed and --fullscreen cannot be given together");
    requireError({"--fullscreen", "--windowed"},
                 "--windowed and --fullscreen cannot be given together");
    requireError({"--windowed=yes"},
                 "--windowed takes a size WxH, each side 1 to 16384, not 'yes'");
}

TEST_CASE("--windowed WxH gives the window's first size, in either form; without it, none") {
    const ParseResult bare = parse({"--windowed"});
    REQUIRE(bare.outcome == ParseOutcome::kRun);
    CHECK_FALSE(bare.options.windowSize.has_value());

    const ParseResult spaced = parse({"--windowed", "1600x900", "--headless"});
    REQUIRE(spaced.outcome == ParseOutcome::kRun);
    REQUIRE(spaced.options.windowSize.has_value());
    CHECK(spaced.options.windowSize->width == 1600);
    CHECK(spaced.options.windowSize->height == 900);
    CHECK(spaced.options.headless);

    const ParseResult inlined = parse({"--windowed=800x500"});
    REQUIRE(inlined.outcome == ParseOutcome::kRun);
    REQUIRE(inlined.options.windowSize.has_value());
    CHECK(inlined.options.windowSize->width == 800);
    CHECK(inlined.options.windowSize->height == 500);

    const ParseResult largest = parse({"--windowed", "16384x1"});
    REQUIRE(largest.outcome == ParseOutcome::kRun);
    REQUIRE(largest.options.windowSize.has_value());
    CHECK(largest.options.windowSize->width == 16384);
    CHECK(largest.options.windowSize->height == 1);

    requireError({"--windowed", "0x600"},
                 "--windowed takes a size WxH, each side 1 to 16384, not '0x600'");
    requireError({"--windowed", "16385x600"},
                 "--windowed takes a size WxH, each side 1 to 16384, not '16385x600'");
    requireError({"--windowed", "800x"},
                 "--windowed takes a size WxH, each side 1 to 16384, not '800x'");
    requireError({"--windowed", "800X600"},
                 "--windowed takes a size WxH, each side 1 to 16384, not '800X600'");
    requireError({"--windowed="}, "--windowed needs [WxH] after '=', not an empty value");
    requireError({"--fullscreen", "--windowed", "800x600"},
                 "--windowed and --fullscreen cannot be given together");
}

TEST_CASE("--fix sets named fixes on or off, in order, each name once") {
    const ParseResult result =
        parse({"--fix", "fix_shield_overflow=on", "--fix=fix_tractor_drift=off"});
    REQUIRE(result.outcome == ParseOutcome::kRun);
    REQUIRE(result.options.fixes.size() == 2);
    CHECK(result.options.fixes[0].name == "fix_shield_overflow");
    CHECK(result.options.fixes[0].on);
    CHECK(result.options.fixes[1].name == "fix_tractor_drift");
    CHECK_FALSE(result.options.fixes[1].on);

    requireError({"--fix", "fix_a=on", "--fix", "fix_a=off"}, "--fix fix_a is given twice");
    requireError({"--fix", "fix_a"},
                 "--fix takes NAME=on or NAME=off, with NAME in lower_snake_case, not 'fix_a'");
    requireError({"--fix", "fix_a=yes"},
                 "--fix takes NAME=on or NAME=off, with NAME in lower_snake_case, not 'fix_a=yes'");
    requireError({"--fix", "FixA=on"},
                 "--fix takes NAME=on or NAME=off, with NAME in lower_snake_case, not 'FixA=on'");
    requireError({"--fix", "=on"},
                 "--fix takes NAME=on or NAME=off, with NAME in lower_snake_case, not '=on'");
    requireError({"--fix"}, "--fix needs NAME=on|off");
}

TEST_CASE("--oracle needs --input; --input alone plays a script with no trace") {
    const ParseResult result = parse({"--oracle", "skirmish.trace", "--input", "skirmish.input",
                                      "--headless", "--watchdog", "120"});
    REQUIRE(result.outcome == ParseOutcome::kRun);
    CHECK(*result.options.oracleTrace == "skirmish.trace");
    CHECK(*result.options.inputScript == "skirmish.input");
    CHECK(result.options.headless);
    CHECK(result.options.watchdogSeconds == 120);

    requireError({"--oracle", "skirmish.trace"}, "--oracle needs --input SCRIPT as well");
    const ParseResult menuRun = parse({"--input", "menu.steps", "--windowed"});
    REQUIRE(menuRun.outcome == ParseOutcome::kRun);
    CHECK(*menuRun.options.inputScript == "menu.steps");
    CHECK_FALSE(menuRun.options.oracleTrace.has_value());
    requireError({"--oracle"}, "--oracle needs TRACE");
    requireError({"--oracle", "a.trace", "--input"}, "--input needs SCRIPT");
}

TEST_CASE("--headless takes no value") {
    CHECK(parse({"--headless"}).options.headless);
    requireError({"--headless=1"}, "--headless takes no value");
    requireError({"--headless", "--headless"}, "--headless is given twice");
}

TEST_CASE("--watchdog takes whole seconds from 0 to 86400") {
    CHECK(parse({"--watchdog", "0"}).options.watchdogSeconds == 0);
    CHECK(parse({"--watchdog", "30"}).options.watchdogSeconds == 30);
    CHECK(parse({"--watchdog=86400"}).options.watchdogSeconds == 86400);
    requireError({"--watchdog", "86401"},
                 "--watchdog takes a whole number of seconds from 0 to 86400, not '86401'");
    requireError({"--watchdog", "1.5"},
                 "--watchdog takes a whole number of seconds from 0 to 86400, not '1.5'");
    requireError({"--watchdog", "-3"}, "--watchdog takes a whole number of seconds from 0 to "
                                       "86400, not '-3'");
    requireError({"--watchdog", "9999999999"},
                 "--watchdog takes a whole number of seconds from 0 to 86400, not '9999999999'");
    requireError({"--watchdog"}, "--watchdog needs S");
    // The help states the same range.
    CHECK(sfcop::app::helpText().find("  --watchdog S          stop after S seconds, 0 to 86400 "
                                      "(0, the default: never)\n") != std::string::npos);
}

TEST_CASE("--fixed-step takes milliseconds per frame, to the microsecond") {
    CHECK_FALSE(parse({}).options.fixedStepMicroseconds);
    CHECK(parse({"--fixed-step", "16.667"}).options.fixedStepMicroseconds == 16667u);
    CHECK(parse({"--fixed-step=20"}).options.fixedStepMicroseconds == 20000u);
    CHECK(parse({"--fixed-step", "0.001"}).options.fixedStepMicroseconds == 1u);
    CHECK(parse({"--fixed-step", "1000"}).options.fixedStepMicroseconds == 1000000u);
    CHECK(parse({"--fixed-step", "33.3"}).options.fixedStepMicroseconds == 33300u);
    CHECK(parse({"--fixed-step", "5."}).options.fixedStepMicroseconds == 5000u);
    const char *const kRange = "--fixed-step takes milliseconds above 0 and up to 1000, with at "
                               "most three decimals, not '";
    requireError({"--fixed-step", "0"}, std::string(kRange) + "0'");
    requireError({"--fixed-step", "1000.001"}, std::string(kRange) + "1000.001'");
    requireError({"--fixed-step", "16.6667"}, std::string(kRange) + "16.6667'");
    requireError({"--fixed-step", "1.2.3"}, std::string(kRange) + "1.2.3'");
    requireError({"--fixed-step", "."}, std::string(kRange) + ".'");
    requireError({"--fixed-step", "-5"}, std::string(kRange) + "-5'");
    requireError({"--fixed-step", "99999999999"}, std::string(kRange) + "99999999999'");
    requireError({"--fixed-step"}, "--fixed-step needs MS");
    requireError({"--fixed-step", "10", "--fixed-step", "20"}, "--fixed-step is given twice");
}

TEST_CASE("an unknown flag is bad input and is named") {
    requireError({"--bogus"}, "unknown flag '--bogus'");
    requireError({"--headless", "--bogus=3"}, "unknown flag '--bogus'");
    requireError({"-x"}, "unexpected argument '-x'");
    requireError({"stray"}, "unexpected argument 'stray'");
}

TEST_CASE("--help and -h ask for the help") {
    CHECK(parse({"--help"}).outcome == ParseOutcome::kHelp);
    CHECK(parse({"-h"}).outcome == ParseOutcome::kHelp);
    CHECK(parse({"--headless", "--help"}).exitCode() == 0);
}

TEST_CASE("the argc and argv form skips the program name") {
    const char *argv[] = {"sfcop", "--install", "/games/op", "--headless"};
    const ParseResult result = parseCommandLine(4, argv);
    REQUIRE(result.outcome == ParseOutcome::kRun);
    CHECK(*result.options.installDir == "/games/op");
    CHECK(result.options.headless);
}

TEST_CASE("the caller's extra flags pass through verbatim and in order") {
    const std::vector<ExtraFlag> extras = {{"--game", "DIR", "a writable copy"},
                                           {"--trace", "", "log every call"}};
    const ParseResult result =
        parseCommandLine({"--trace", "--headless", "--game=/copy", "--trace"}, extras);
    REQUIRE(result.outcome == ParseOutcome::kRun);
    CHECK(result.options.headless);
    CHECK(result.options.passThrough ==
          std::vector<std::string>{"--trace", "--game", "/copy", "--trace"});

    const ParseResult missing = parseCommandLine({"--game"}, extras);
    CHECK(missing.outcome == ParseOutcome::kError);
    CHECK(missing.error == "--game needs DIR");
}

TEST_CASE("--help lists every flag of DESIGN 6.8 and every exit code") {
    const std::string help = sfcop::app::helpText();
    for (const char *line : {
             "  --install DIR ",
             "  --data DIR ",
             "  --logs DIR ",
             "  --windowed ",
             "  --fullscreen ",
             "  --fix NAME=on|off ",
             "  --oracle TRACE ",
             "  --input SCRIPT        play the steps file SCRIPT into the game (--oracle needs it; the "
             "window is then read only when the game peeks)\n",
             "  --headless ",
             "  --watchdog S ",
             "  --help, -h ",
             "  0  success\n",
             "  2  bad input on the command line\n",
             "  3  install not found or verification failed\n",
             "  4  oracle mismatch\n",
         }) {
        CAPTURE(line);
        CHECK(help.find(line) != std::string::npos);
    }
    CHECK(help.rfind("usage: sfcop [options]\n", 0) == 0);
    CHECK(help.find("other options:") == std::string::npos);
}

TEST_CASE("a run started by hand reads the window during loads; a scripted, recorded or headless one does not") {
    const std::vector<ExtraFlag> extras = {{"--record-trace", "OUT", "record"}};
    const auto reads = [&](std::vector<std::string> args) {
        const ParseResult result = parseCommandLine(args, extras);
        REQUIRE(result.outcome == ParseOutcome::kRun);
        return readsWindowDuringLoads(result.options);
    };
    CHECK(reads({}));
    CHECK(reads({"--windowed"}));
    CHECK(reads({"--fixed-step", "16.667"})); // a clock mode, not a recording
    CHECK_FALSE(reads({"--input", "menu.steps"}));
    CHECK_FALSE(reads({"--input", "skirmish.steps", "--oracle", "a/b/skirmish.trace"}));
    sfcop::app::ClientOptions oracleOnly; // the parser refuses --oracle without --input
    oracleOnly.oracleTrace = "a/b/skirmish.trace";
    CHECK_FALSE(readsWindowDuringLoads(oracleOnly));
    CHECK_FALSE(reads({"--record-trace", "out/OracleSkirmish/skirmish.trace"}));
    CHECK_FALSE(reads({"--headless"}));
}

TEST_CASE("--help lists the caller's extra flags under their own heading") {
    const std::string help = sfcop::app::helpText({{"--game", "DIR", "a writable copy"}});
    CHECK(help.find("\nother options:\n  --game DIR            a writable copy\n") !=
          std::string::npos);
}

TEST_CASE("--render-scale takes a whole number from 0 to 16, 0 when absent") {
    CHECK(parse({}).options.renderScale == 0);
    CHECK(parse({"--render-scale", "0"}).options.renderScale == 0);
    CHECK(parse({"--render-scale", "1"}).options.renderScale == 1);
    CHECK(parse({"--render-scale", "16"}).options.renderScale == 16);
    CHECK(parse({"--render-scale", "17"}).error ==
          "--render-scale takes a whole number from 0 to 16, not '17'");
    CHECK(parse({"--render-scale", "x"}).error ==
          "--render-scale takes a whole number from 0 to 16, not 'x'");
    CHECK(parse({"--render-scale", "-1"}).error ==
          "--render-scale takes a whole number from 0 to 16, not '-1'");
}

TEST_CASE("--import takes a folder and cannot go with --install") {
    const ParseResult result = parse({"--import", "/games/op", "--data", "/d"});
    REQUIRE(result.outcome == ParseOutcome::kRun);
    CHECK(result.options.importFrom == std::optional<std::string>("/games/op"));
    CHECK_FALSE(parse({}).options.importFrom.has_value());
    requireError({"--import"}, "--import needs DIR");
    requireError({"--import", "/a", "--install", "/b"},
                 "--import and --install cannot be given together");
    CHECK(sfcop::app::helpText().find("  --import DIR") != std::string::npos);
}

TEST_CASE("--verify-import takes no value and goes with neither --import nor --install") {
    const ParseResult result = parse({"--verify-import", "--data", "/d"});
    REQUIRE(result.outcome == ParseOutcome::kRun);
    CHECK(result.options.verifyImport);
    CHECK_FALSE(parse({}).options.verifyImport);
    requireError({"--verify-import", "--import", "/a"},
                 "--verify-import goes with neither --import nor --install");
    requireError({"--verify-import", "--install", "/a"},
                 "--verify-import goes with neither --import nor --install");
    CHECK(sfcop::app::helpText().find("  --verify-import ") != std::string::npos);
}
