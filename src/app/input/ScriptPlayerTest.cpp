#include "app/input/Marks.h"
#include "app/input/ScriptPlayer.h"

#include <doctest/doctest.h>

#include <set>

using sfcop::app::input::parseInputScript;
using sfcop::app::input::PlayerEnvironment;
using sfcop::app::input::ScriptPlayer;
using sfcop::platform::InputEventType;
using sfcop::platform::WindowEvent;
using sfcop::platform::WindowEventType;

namespace {

ScriptPlayer playerFor(const char *text, PlayerEnvironment environment = {}) {
    const auto parsed = parseInputScript(text);
    REQUIRE_FALSE(parsed.error.has_value());
    return ScriptPlayer(parsed.script, std::move(environment));
}

} // namespace

TEST_CASE("a click is a move, a button down and a button up in three successive frames") {
    ScriptPlayer player = playerFor("click 157 172\n");
    const std::vector<WindowEvent> first = player.Advance(1, 100);
    REQUIRE(first.size() == 1);
    CHECK(first[0].type == WindowEventType::kMouseMove);
    CHECK(first[0].timeMs == 100);
    CHECK(first[0].input.type == InputEventType::kMouseMove);
    CHECK(first[0].input.x == 157);
    CHECK(first[0].input.y == 172);
    CHECK(first[0].input.dx == 157);
    CHECK(first[0].input.dy == 172);
    CHECK(player.Advance(1, 101).empty()); // the same frame again: nothing more

    const std::vector<WindowEvent> second = player.Advance(2, 116);
    REQUIRE(second.size() == 1);
    CHECK(second[0].type == WindowEventType::kMouseButtonDown);
    CHECK(second[0].input.type == InputEventType::kMouseButtonDown);
    CHECK(second[0].input.button == sfcop::platform::MouseButton::kLeft);
    CHECK(second[0].input.x == 157);
    CHECK(second[0].input.y == 172);
    CHECK(second[0].timeMs == 116);
    CHECK_FALSE(player.Finished());

    const std::vector<WindowEvent> third = player.Advance(3, 133);
    REQUIRE(third.size() == 1);
    CHECK(third[0].type == WindowEventType::kMouseButtonUp);
    CHECK(third[0].input.type == InputEventType::kMouseButtonUp);
    CHECK(third[0].input.x == 157);
    CHECK(player.Finished());
    CHECK(player.Advance(4, 150).empty());
}

TEST_CASE("a key is its usage ID down, then up in the next frame") {
    ScriptPlayer player = playerFor("key 0x1B\n");
    const auto down = player.Advance(10, 500);
    REQUIRE(down.size() == 1);
    CHECK(down[0].type == WindowEventType::kKeyDown);
    CHECK(down[0].input.type == InputEventType::kKeyDown);
    CHECK(down[0].input.key == 0x29);
    const auto up = player.Advance(11, 517);
    REQUIRE(up.size() == 1);
    CHECK(up[0].type == WindowEventType::kKeyUp);
    CHECK(up[0].input.key == 0x29);
    CHECK(player.Finished());
}

TEST_CASE("sleep waits by the clock and frames by the frame count") {
    ScriptPlayer player = playerFor("sleep 50\nclick 1 2\nframes 3\nkey 13\n");
    CHECK(player.Advance(1, 1000).empty());
    CHECK(player.Advance(2, 1049).empty());
    const auto moved = player.Advance(3, 1050); // 50 ms on: the click starts in this frame
    REQUIRE(moved.size() == 1);
    CHECK(moved[0].type == WindowEventType::kMouseMove);
    CHECK(player.Advance(4, 1066).size() == 1); // down
    CHECK(player.Advance(5, 1083).size() == 1); // up; `frames 3` starts in frame 5
    CHECK(player.Advance(6, 1100).empty());
    CHECK(player.Advance(7, 1116).empty());
    const auto key = player.Advance(8, 1133);   // three frames after frame 5
    REQUIRE(key.size() == 1);
    CHECK(key[0].type == WindowEventType::kKeyDown);
    CHECK(key[0].input.key == 0x28);
}

TEST_CASE("two clicks in a row never share a frame") {
    ScriptPlayer player = playerFor("click 10 10\nclick 30 40\n");
    std::vector<WindowEventType> seen;
    for (std::uint64_t frame = 1; frame <= 6; ++frame) {
        const auto events = player.Advance(frame, static_cast<std::uint32_t>(frame * 16));
        CHECK(events.size() == 1);
        for (const WindowEvent &event : events) {
            seen.push_back(event.type);
        }
    }
    CHECK(seen == std::vector<WindowEventType>{
                      WindowEventType::kMouseMove, WindowEventType::kMouseButtonDown,
                      WindowEventType::kMouseButtonUp, WindowEventType::kMouseMove,
                      WindowEventType::kMouseButtonDown, WindowEventType::kMouseButtonUp});
    CHECK(player.Finished());
}

TEST_CASE("waitmark waits for the mark the guest host records") {
    sfcop::app::input::ForgetMarks();
    PlayerEnvironment environment;
    environment.markReached = [](const std::string &mark) {
        return sfcop::app::input::MarkReached(mark);
    };
    ScriptPlayer player = playerFor("waitmark 30 Setup\nclick 5 6\n", environment);
    CHECK(player.Advance(1, 0).empty());
    sfcop::app::input::NoteMark(0x0069A840u, "WinMain");
    CHECK(player.Advance(2, 4000).empty());
    sfcop::app::input::NoteMark(0x0047C155u, "Setup");
    const auto events = player.Advance(3, 7000);
    REQUIRE(events.size() == 1);
    CHECK(events[0].type == WindowEventType::kMouseMove);
    CHECK_FALSE(player.Failure().has_value());
    sfcop::app::input::ForgetMarks();
}

TEST_CASE("a mark is found by its name or its address in any case") {
    sfcop::app::input::ForgetMarks();
    CHECK_FALSE(sfcop::app::input::MarkReached("Setup"));
    sfcop::app::input::NoteMark(0x0047C155u, "Setup");
    sfcop::app::input::NoteMark(0x0047B861u, "");
    CHECK(sfcop::app::input::MarkReached("Setup"));
    CHECK(sfcop::app::input::MarkReached("0x0047C155"));
    CHECK(sfcop::app::input::MarkReached("0x0047b861"));
    CHECK_FALSE(sfcop::app::input::MarkReached("setup"));
    CHECK_FALSE(sfcop::app::input::MarkReached(""));
    CHECK_FALSE(sfcop::app::input::MarkReached("0x00400000"));
    sfcop::app::input::ForgetMarks();
    CHECK_FALSE(sfcop::app::input::MarkReached("Setup"));
}

TEST_CASE("a wait that runs out fails the script with its line and what it waited for") {
    ScriptPlayer player = playerFor("sleep 0\nwaitmark 2 Setup\nclick 1 1\n",
                                    PlayerEnvironment{{}, [](const std::string &) { return false; }, {}});
    CHECK(player.Advance(1, 100).empty());
    CHECK(player.Advance(2, 2099).empty());
    CHECK_FALSE(player.Failure().has_value());
    CHECK(player.Advance(3, 2100).empty());
    REQUIRE(player.Failure().has_value());
    CHECK(player.Failure()->line == 2);
    CHECK(player.Failure()->message == "waited 2 s for the mark Setup; it did not come");
    CHECK(player.Advance(4, 5000).empty());
    CHECK_FALSE(player.Finished());
}

TEST_CASE("waitfile asks the environment and times out naming the file") {
    std::set<std::string> files;
    PlayerEnvironment environment;
    environment.fileExists = [&files](const std::string &path) { return files.count(path) != 0; };
    ScriptPlayer player = playerFor("waitfile 1 OracleTrace.txt\n", environment);
    CHECK(player.Advance(1, 0).empty());
    files.insert("OracleTrace.txt");
    CHECK(player.Advance(2, 500).empty());
    CHECK(player.Finished());

    ScriptPlayer late = playerFor("waitfile 1 Missing.txt\n", environment);
    late.Advance(1, 0);
    late.Advance(2, 1000);
    REQUIRE(late.Failure().has_value());
    CHECK(late.Failure()->message == "waited 1 s for the file Missing.txt; it did not come");
}

TEST_CASE("each step is logged as it starts; the Wine driver's shot and list are skipped") {
    std::vector<std::string> lines;
    PlayerEnvironment environment;
    environment.log = [&lines](const std::string &line) { lines.push_back(line); };
    ScriptPlayer player = playerFor("window 60 Starfleet Command\nshot a.bmp\nlist\n", environment);
    CHECK(player.Advance(7, 1234).empty());
    CHECK(player.Finished());
    CHECK(lines == std::vector<std::string>{
                       "line 1 at 1234 ms, frame 7: window 60 Starfleet Command",
                       "line 2 at 1234 ms, frame 7: shot a.bmp",
                       "line 2: shot is the Wine driver's; skipped (--shots takes the native "
                       "client's captures)",
                       "line 3 at 1234 ms, frame 7: list",
                       "line 3: list is the Wine driver's; skipped",
                   });
}
