#include "app/CommandLine.h"

#include <doctest/doctest.h>

using sfcop::app::ExtraFlag;
using sfcop::app::parseCommandLine;
using sfcop::app::ParseOutcome;
using sfcop::app::ParseResult;
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

TEST_CASE("--windowed and --fullscreen choose the window mode and exclude each other") {
    CHECK(parse({"--windowed"}).options.window == WindowMode::kWindowed);
    CHECK(parse({"--fullscreen"}).options.window == WindowMode::kFullscreen);
    requireError({"--windowed", "--fullscreen"},
                 "--windowed and --fullscreen cannot be given together");
    requireError({"--fullscreen", "--windowed"},
                 "--windowed and --fullscreen cannot be given together");
    requireError({"--windowed=yes"}, "--windowed takes no value");
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
             "  --windowed ",
             "  --fullscreen ",
             "  --fix NAME=on|off ",
             "  --oracle TRACE ",
             "  --input SCRIPT        play the steps file SCRIPT into the game (--oracle needs it)\n",
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

TEST_CASE("--help lists the caller's extra flags under their own heading") {
    const std::string help = sfcop::app::helpText({{"--game", "DIR", "a writable copy"}});
    CHECK(help.find("\nother options:\n  --game DIR            a writable copy\n") !=
          std::string::npos);
}
