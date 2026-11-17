#include "fixes/fixes.h"

#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using sfcop::fixes::Kind;
using sfcop::fixes::Origin;
using sfcop::fixes::Problem;
using sfcop::fixes::Settings;
using sfcop::fixes::Source;
using sfcop::fixes::Switch;

// A list in the switches.def form, read the way fixes.h reads it: the build switches' blocks.
#define SFCOP_FIX_SWITCH(name, kind, on, origin, description)
#include "fixes/FixesTestSwitches.def"
#undef SFCOP_FIX_SWITCH

// A build switch's block holds the default its line gives; a runtime switch has no block.
#define SFCOP_FIX_TEST_AGREES_Build(name, on) CHECK(SFCOP_FIX_BUILD_##name == (on));
#define SFCOP_FIX_TEST_AGREES_Runtime(name, on)
// The starting position, as Fixes.cpp builds the registry.
#define SFCOP_FIX_TEST_START_Runtime(name, on) ((on) != 0)
#define SFCOP_FIX_TEST_START_Build(name, on) (SFCOP_FIX(name) != 0)

namespace {

#if SFCOP_FIX(fix_test_def_on)
constexpr int kDefOnRegion = 1;  // the fix
#else
constexpr int kDefOnRegion = 0;  // the original
#endif

#if SFCOP_FIX(fix_test_def_off)
constexpr int kDefOffRegion = 1;
#else
constexpr int kDefOffRegion = 0;
#endif

std::vector<Switch> defSwitches() {
    std::vector<Switch> v;
#define SFCOP_FIX_SWITCH(name, kind, on, origin, description)                                    \
    v.push_back({#name, SFCOP_FIX_TEST_START_##kind(name, on), Origin::origin, Kind::kind,         \
                 description});
#include "fixes/FixesTestSwitches.def"
#undef SFCOP_FIX_SWITCH
    return v;
}

// A crash fix (default on), a rules change (default off) and a build switch compiled on.
std::vector<Switch> testSwitches() {
    return {
        {"fix_test_built", true, Origin::Project, Kind::Build, "a build switch compiled on"},
        {"fix_test_crash", true, Origin::Project, Kind::Runtime, "a crash fix"},
        {"fix_test_rules", false, Origin::Community, Kind::Runtime, "a rules change"},
    };
}

}  // namespace

TEST_CASE("SFCOP_FIX in #if selects the fix or the original as the build switch is declared") {
    CHECK(kDefOnRegion == 1);
    CHECK(kDefOffRegion == 0);
    CHECK(SFCOP_FIX(fix_test_def_on) == 1);
    CHECK(SFCOP_FIX(fix_test_def_off) == 0);
}

TEST_CASE("a list in the switches.def form gives each switch its kind, start and origin") {
    const std::vector<Switch> v = defSwitches();
    REQUIRE(v.size() == 3);
    CHECK(v[0].name == "fix_test_def_on");
    CHECK(v[0].kind == Kind::Build);
    CHECK(v[0].defaultOn);
    CHECK(v[1].name == "fix_test_def_off");
    CHECK(v[1].kind == Kind::Build);
    CHECK(!v[1].defaultOn);
    CHECK(v[2].name == "fix_test_def_rules");
    CHECK(v[2].kind == Kind::Runtime);
    CHECK(!v[2].defaultOn);
    CHECK(v[2].origin == Origin::Community);
    CHECK(v[2].description == "a rules change, default off");

    Settings s(v);
    CHECK(s.report() ==
          "fix_test_def_on on (default) [build]\n"
          "fix_test_def_off off (default) [build]\n"
          "fix_test_def_rules off (default)\n");
}

TEST_CASE("each build switch's block holds the default its line gives") {
#define SFCOP_FIX_SWITCH(name, kind, on, origin, description) SFCOP_FIX_TEST_AGREES_##kind(name, on)
#include "fixes/FixesTestSwitches.def"
#include "fixes/switches.def"
#undef SFCOP_FIX_SWITCH
}

namespace {

// The registry's switch of that name, or nullptr.
const Switch *declared(std::string_view name) {
    for (const Switch &s : sfcop::fixes::registry()) {
        if (s.name == name) {
            return &s;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("the registry declares the empty-name object fix as a runtime project fix, default on") {
    const Switch *fix = declared("fix_hle_named_objects");
    REQUIRE(fix != nullptr);
    CHECK(fix->defaultOn);
    CHECK(fix->origin == Origin::Project);
    CHECK(fix->kind == Kind::Runtime);
    CHECK(!fix->description.empty());
}

TEST_CASE("the registry declares the scaled frame's filter as a runtime project fix, default on") {
    const Switch *fix = declared("fix_smooth_scaled_frame");
    REQUIRE(fix != nullptr);
    CHECK(fix->defaultOn);
    CHECK(fix->origin == Origin::Project);
    CHECK(fix->kind == Kind::Runtime);
    CHECK(!fix->description.empty());
}

TEST_CASE("the registry declares the low-memory read fix as a runtime project fix, default on") {
    const Switch *fix = declared("fix_hle_low_memory_reads");
    REQUIRE(fix != nullptr);
    CHECK(fix->defaultOn);
    CHECK(fix->origin == Origin::Project);
    CHECK(fix->kind == Kind::Runtime);
    CHECK(!fix->description.empty());
}

TEST_CASE("every declared switch has a unique lower_snake_case name starting fix_") {
    std::set<std::string> seen;
    for (const Switch &s : sfcop::fixes::registry()) {
        const std::string name(s.name);
        CHECK(name.rfind("fix_", 0) == 0);
        CHECK(name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == std::string::npos);
        CHECK(seen.insert(name).second);
    }
}

TEST_CASE("each switch starts at its default") {
    Settings s(testSwitches());
    CHECK(s.isEnabled("fix_test_crash"));
    CHECK(!s.isEnabled("fix_test_rules"));
    CHECK(s.isEnabled("fix_test_built"));
    CHECK(s.sourceOf("fix_test_crash") == Source::Default);
    CHECK(s.report() ==
          "fix_test_built on (default) [build]\n"
          "fix_test_crash on (default)\n"
          "fix_test_rules off (default)\n");

    Settings real;
    CHECK(real.isEnabled("fix_hle_named_objects"));
    const std::string report = real.report();
    CHECK(report.find("fix_hle_named_objects on (default)\n") != std::string::npos);
    CHECK(report.find("fix_smooth_scaled_frame on (default)\n") != std::string::npos);
}

TEST_CASE("--fix turns a switch off and another on") {
    Settings s(testSwitches());
    CHECK(s.applyCommandLine({"fix_test_crash=off", "fix_test_rules=on"}).empty());
    CHECK(!s.isEnabled("fix_test_crash"));
    CHECK(s.isEnabled("fix_test_rules"));
    CHECK(s.sourceOf("fix_test_rules") == Source::CommandLine);
    CHECK(s.report() ==
          "fix_test_built on (default) [build]\n"
          "fix_test_crash off (--fix)\n"
          "fix_test_rules on (--fix)\n");
}

TEST_CASE("--fix accepts 1 and 0 and the last value for a name wins") {
    Settings s(testSwitches());
    CHECK(s.applyCommandLine({"fix_test_rules=1", "fix_test_crash=0", "fix_test_rules=off"})
              .empty());
    CHECK(!s.isEnabled("fix_test_crash"));
    CHECK(!s.isEnabled("fix_test_rules"));
}

TEST_CASE("settings/fixes.gf turns switches over in the original's .gf form") {
    Settings s(testSwitches());
    const char *text =
        "// fixes.gf\r\n"
        "\r\n"
        "fix_test_crash\t\t= off\t// a comment\r\n"
        "fix_test_rules = \"1\"\r\n";
    CHECK(s.applyFile(text, "fixes.gf").empty());
    CHECK(!s.isEnabled("fix_test_crash"));
    CHECK(s.isEnabled("fix_test_rules"));
    CHECK(s.sourceOf("fix_test_crash") == Source::File);
    CHECK(s.report() ==
          "fix_test_built on (default) [build]\n"
          "fix_test_crash off (settings)\n"
          "fix_test_rules on (settings)\n");
}

TEST_CASE("a fixes.gf on disk is read, and a missing one is no override") {
    const std::filesystem::path path = "FixesTest_fixes.gf";
    {
        std::ofstream out(path, std::ios::binary);
        out << "fix_test_rules = on\n";
    }
    Settings s(testSwitches());
    CHECK(s.loadFile(path).empty());
    CHECK(s.isEnabled("fix_test_rules"));
    std::filesystem::remove(path);

    Settings untouched(testSwitches());
    CHECK(untouched.loadFile("FixesTest_no_such_file.gf").empty());
    CHECK(!untouched.isEnabled("fix_test_rules"));
}

TEST_CASE("the command line wins over the file, and the file over the default, in either order") {
    const char *file = "fix_test_crash = off\nfix_test_rules = on\n";

    Settings fileFirst(testSwitches());
    CHECK(fileFirst.applyFile(file, "fixes.gf").empty());
    CHECK(fileFirst.applyCommandLine({"fix_test_crash=on"}).empty());

    Settings commandFirst(testSwitches());
    CHECK(commandFirst.applyCommandLine({"fix_test_crash=on"}).empty());
    CHECK(commandFirst.applyFile(file, "fixes.gf").empty());

    for (const Settings *s : {&fileFirst, &commandFirst}) {
        CHECK(s->isEnabled("fix_test_crash"));                     // --fix over the file
        CHECK(s->sourceOf("fix_test_crash") == Source::CommandLine);
        CHECK(s->isEnabled("fix_test_rules"));                     // the file over the default
        CHECK(s->sourceOf("fix_test_rules") == Source::File);
        CHECK(s->isEnabled("fix_test_built"));                     // the default
    }
}

TEST_CASE("an unknown name is rejected on the command line and in the file, and changes nothing") {
    Settings s(testSwitches());
    const std::vector<Problem> command =
        s.applyCommandLine({"fix_test_rules=on", "fix_nonesuch=on"});
    REQUIRE(command.size() == 1);
    CHECK(command[0].source == "--fix");
    CHECK(command[0].line == 2);
    CHECK(command[0].message == "unknown fix switch 'fix_nonesuch'");
    CHECK(s.isEnabled("fix_test_rules"));  // the good value still applies

    const std::vector<Problem> file =
        s.applyFile("fix_test_crash = off\nfix_nonesuch = off\n", "fixes.gf");
    REQUIRE(file.size() == 1);
    CHECK(file[0].source == "fixes.gf");
    CHECK(file[0].line == 2);
    CHECK(file[0].message == "unknown fix switch 'fix_nonesuch'");

    CHECK(!s.isDeclared("fix_nonesuch"));
    CHECK(!s.isEnabled("fix_nonesuch"));
}

TEST_CASE("a malformed --fix value is rejected") {
    Settings s(testSwitches());
    const std::vector<Problem> p = s.applyCommandLine({"fix_test_crash", "fix_test_crash=maybe"});
    REQUIRE(p.size() == 2);
    CHECK(p[0].message == "'fix_test_crash' is not NAME=on|off");
    CHECK(p[1].line == 2);
    CHECK(p[1].message == "'fix_test_crash=maybe' is not NAME=on|off");
    CHECK(s.isEnabled("fix_test_crash"));
}

TEST_CASE("a malformed fixes.gf line is reported with its line number and skipped") {
    Settings s(testSwitches());
    const std::vector<Problem> p = s.applyFile(
        "[Fixes]\n"
        "fix_test_crash off\n"
        "fix_test_crash = sometimes\n"
        "fix_test_rules = on\n",
        "fixes.gf");
    REQUIRE(p.size() == 3);
    CHECK(p[0].line == 1);
    CHECK(p[0].message == "a section line '[Fixes]'; fixes.gf has no sections");
    CHECK(p[1].line == 2);
    CHECK(p[1].message == "no '=' in 'fix_test_crash off'");
    CHECK(p[2].line == 3);
    CHECK(p[2].message == "'sometimes' for fix_test_crash is not on, off, 1 or 0");
    CHECK(s.isEnabled("fix_test_crash"));
    CHECK(s.isEnabled("fix_test_rules"));
}

TEST_CASE("a build switch keeps its compiled position") {
    Settings s(testSwitches());
    CHECK(s.applyCommandLine({"fix_test_built=on"}).empty());
    const std::vector<Problem> p = s.applyCommandLine({"fix_test_built=off"});
    REQUIRE(p.size() == 1);
    CHECK(p[0].message ==
          "fix_test_built is a build switch compiled on; it changes only by rebuilding");
    CHECK(s.isEnabled("fix_test_built"));
}

TEST_CASE("the installed set answers isEnabled for the empty-name object fix in both positions") {
    CHECK(sfcop::fixes::isEnabled("fix_hle_named_objects"));

    Settings off;
    CHECK(off.applyCommandLine({"fix_hle_named_objects=off"}).empty());
    sfcop::fixes::install(off);
    CHECK(!sfcop::fixes::isEnabled("fix_hle_named_objects"));
    CHECK(sfcop::fixes::current().sourceOf("fix_hle_named_objects") == Source::CommandLine);

    sfcop::fixes::install(Settings());
    CHECK(sfcop::fixes::isEnabled("fix_hle_named_objects"));
    CHECK(!sfcop::fixes::isEnabled("fix_nonesuch"));
}
