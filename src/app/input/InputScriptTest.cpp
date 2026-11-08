#include "app/input/InputScript.h"
#include "app/input/VirtualKeys.h"

#include <doctest/doctest.h>

using sfcop::app::input::parseInputScript;
using sfcop::app::input::ParsedScript;
using sfcop::app::input::StepKind;

namespace {

void requireRefused(const char *text, unsigned line, const std::string &message) {
    const ParsedScript parsed = parseInputScript(text);
    REQUIRE(parsed.error.has_value());
    CHECK(parsed.error->line == line);
    CHECK(parsed.error->message == message);
    CHECK(parsed.script.steps.empty());
    CHECK(describe(*parsed.error) == "line " + std::to_string(line) + ": " + message);
}

} // namespace

TEST_CASE("a steps file in the Wine driver's vocabulary reads step by step, comments skipped") {
    const ParsedScript parsed = parseInputScript("# boot to the menu\n"
                                                 "\n"
                                                 "window 120 Starfleet Command\n"
                                                 "sleep 15000\r\n"
                                                 "  # indented comment\n"
                                                 "click 157 172\n"
                                                 "key 0x1B\n"
                                                 "key 13\n"
                                                 "shot menu.bmp\n"
                                                 "waitfile 300 C:\\Games\\SFCOP\\OracleTrace.txt\n"
                                                 "list");
    REQUIRE_FALSE(parsed.error.has_value());
    const auto &steps = parsed.script.steps;
    REQUIRE(steps.size() == 8);
    CHECK(steps[0].kind == StepKind::kWindow);
    CHECK(steps[0].line == 3);
    CHECK(steps[0].milliseconds == 120000);
    CHECK(steps[0].text == "Starfleet Command");
    CHECK(steps[1].kind == StepKind::kSleep);
    CHECK(steps[1].line == 4);
    CHECK(steps[1].milliseconds == 15000);
    CHECK(steps[2].kind == StepKind::kClick);
    CHECK(steps[2].line == 6);
    CHECK(steps[2].x == 157);
    CHECK(steps[2].y == 172);
    CHECK(steps[3].kind == StepKind::kKey);
    CHECK(steps[3].virtualKey == 27);
    CHECK(steps[4].virtualKey == 13);
    CHECK(steps[5].kind == StepKind::kShot);
    CHECK(steps[5].text == "menu.bmp");
    CHECK(steps[6].kind == StepKind::kWaitFile);
    CHECK(steps[6].milliseconds == 300000);
    CHECK(steps[6].text == "C:\\Games\\SFCOP\\OracleTrace.txt");
    CHECK(steps[7].kind == StepKind::kList);
    CHECK(steps[7].line == 11);
}

TEST_CASE("the native player's own steps: waitmark and frames") {
    const ParsedScript parsed = parseInputScript("waitmark 30 Setup\n"
                                                 "waitmark 5 0x0047C155\n"
                                                 "frames 2\n");
    REQUIRE_FALSE(parsed.error.has_value());
    const auto &steps = parsed.script.steps;
    REQUIRE(steps.size() == 3);
    CHECK(steps[0].kind == StepKind::kWaitMark);
    CHECK(steps[0].milliseconds == 30000);
    CHECK(steps[0].text == "Setup");
    CHECK(steps[1].text == "0x0047C155");
    CHECK(steps[2].kind == StepKind::kFrames);
    CHECK(steps[2].frames == 2);
}

TEST_CASE("keydown and keyup read a virtual key as key does") {
    const ParsedScript parsed = parseInputScript("keydown 0x10\n"
                                                 "key 0x32\n"
                                                 "keyup 16\n");
    REQUIRE_FALSE(parsed.error.has_value());
    const auto &steps = parsed.script.steps;
    REQUIRE(steps.size() == 3);
    CHECK(steps[0].kind == StepKind::kKeyDown);
    CHECK(steps[0].line == 1);
    CHECK(steps[0].virtualKey == 16);
    CHECK(steps[1].kind == StepKind::kKey);
    CHECK(steps[1].virtualKey == 50);
    CHECK(steps[2].kind == StepKind::kKeyUp);
    CHECK(steps[2].line == 3);
    CHECK(steps[2].virtualKey == 16);
}

TEST_CASE("a malformed keydown or keyup refuses the file with its own verb") {
    requireRefused("keydown\n", 1, "keydown takes one virtual key, 1 to 254, decimal or 0x hex");
    requireRefused("keyup 0x10 0x11\n", 1,
                   "keyup takes one virtual key, 1 to 254, decimal or 0x hex");
    requireRefused("keydown 0x07\n", 1,
                   "keydown 0x07 is a virtual key the player has no keyboard key for");
    requireRefused("sleep 1\nkeyup 0x07\n", 2,
                   "keyup 0x07 is a virtual key the player has no keyboard key for");
}

TEST_CASE("an unknown or malformed step refuses the file, naming its line") {
    requireRefused("sleep 10\njump 3\n", 2, "unknown step 'jump'");
    requireRefused("sleep\n", 1, "sleep takes one whole number of milliseconds, 0 to 86400000");
    requireRefused("sleep 10 20\n", 1,
                   "sleep takes one whole number of milliseconds, 0 to 86400000");
    requireRefused("sleep -5\n", 1, "sleep takes one whole number of milliseconds, 0 to 86400000");
    requireRefused("# c\nclick 10\n", 2, "click takes two coordinates, x and y, each 0 to 16383");
    requireRefused("click 10 20000\n", 1,
                   "click takes two coordinates, x and y, each 0 to 16383");
    requireRefused("key 0\n", 1, "key takes one virtual key, 1 to 254, decimal or 0x hex");
    requireRefused("key 0x100\n", 1, "key takes one virtual key, 1 to 254, decimal or 0x hex");
    requireRefused("key 0x07\n", 1, "key 0x07 is a virtual key the player has no keyboard key for");
    requireRefused("window 120\n", 1,
                   "window takes a timeout of 0 to 86400 seconds and a window title");
    requireRefused("waitmark 86401 Setup\n", 1,
                   "waitmark takes a timeout of 0 to 86400 seconds and a mark's name or address");
    requireRefused("waitfile 3\n", 1, "waitfile takes a timeout of 0 to 86400 seconds and a path");
    requireRefused("frames 0\n", 1, "frames takes one whole number of frames, 1 to 1000000");
    requireRefused("shot\n", 1, "shot takes a file name");
    requireRefused("list all\n", 1, "list takes nothing after it");
}

TEST_CASE("virtual keys map to the USB usage IDs Input uses") {
    using sfcop::app::input::scancodeForVirtualKey;
    CHECK(scancodeForVirtualKey(0x41) == 0x04); // A
    CHECK(scancodeForVirtualKey(0x5A) == 0x1D); // Z
    CHECK(scancodeForVirtualKey(0x31) == 0x1E); // 1
    CHECK(scancodeForVirtualKey(0x30) == 0x27); // 0
    CHECK(scancodeForVirtualKey(0x0D) == 0x28); // Return
    CHECK(scancodeForVirtualKey(0x1B) == 0x29); // Escape
    CHECK(scancodeForVirtualKey(0x20) == 0x2C); // Space
    CHECK(scancodeForVirtualKey(0x70) == 0x3A); // F1
    CHECK(scancodeForVirtualKey(0x7B) == 0x45); // F12
    CHECK(scancodeForVirtualKey(0x26) == 0x52); // Up
    CHECK(scancodeForVirtualKey(0x61) == 0x59); // keypad 1
    CHECK(scancodeForVirtualKey(0x10) == 0xE1); // Shift: left
    CHECK_FALSE(scancodeForVirtualKey(0x07).has_value());
}
