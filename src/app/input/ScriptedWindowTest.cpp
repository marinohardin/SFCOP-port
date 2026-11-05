#include "app/input/ScriptedWindow.h"
#include "platform/Clock/ClockMock.h"
#include "platform/Input/InputMock.h"
#include "platform/Window/WindowMock.h"

#include <doctest/doctest.h>

#include <thread>
#include <utility>

using sfcop::app::input::parseInputScript;
using sfcop::app::input::PlayerFailure;
using sfcop::app::input::ScriptedWindow;
using sfcop::app::input::ScriptPlayer;
using sfcop::platform::ClockMock;
using sfcop::platform::Error;
using sfcop::platform::InputEventType;
using sfcop::platform::InputMock;
using sfcop::platform::WindowEvent;
using sfcop::platform::WindowEventType;
using sfcop::platform::WindowMock;
using sfcop::platform::WindowState;

namespace {

ScriptPlayer playerFor(const char *text) {
    const auto parsed = parseInputScript(text);
    REQUIRE_FALSE(parsed.error.has_value());
    return ScriptPlayer(parsed.script, {});
}

WindowState focusedState() {
    WindowState state;
    state.width = 640;
    state.height = 480;
    state.focused = true;
    return state;
}

// What the guest host's pump does each turn: pump, then poll until empty.
std::vector<WindowEvent> pumpTurn(ScriptedWindow &window) {
    REQUIRE(window.PumpEvents().Ok());
    std::vector<WindowEvent> events;
    for (;;) {
        auto event = window.PollEvent();
        REQUIRE(event.Ok());
        if (!event.Value()) {
            return events;
        }
        events.push_back(*event.Value());
    }
}

WindowEvent realKey(WindowEventType type, std::uint16_t key) {
    WindowEvent event;
    event.type = type;
    event.input.type =
        type == WindowEventType::kKeyDown ? InputEventType::kKeyDown : InputEventType::kKeyUp;
    event.input.key = key;
    return event;
}

} // namespace

TEST_CASE("a steps file plays as one sequence of window and input events, at the frames written") {
    WindowMock inner;
    inner.stateResult = focusedState();
    InputMock input;
    ClockMock clock;
    ScriptPlayer player = playerFor("sleep 20\nclick 157 172\nkey 13\n");
    ScriptedWindow window(inner, player, clock, {});
    window.BindToCallingThread();
    REQUIRE(window.AttachInput(input).Ok());
    CHECK(inner.sink == &input);

    // Frame n at 16 ms per frame, from 0.
    std::vector<std::pair<std::uint64_t, WindowEventType>> seen;
    for (std::uint32_t frame = 1; frame <= 8; ++frame) {
        clock.ticksResult = (frame - 1) * 16;
        for (const WindowEvent &event : pumpTurn(window)) {
            seen.emplace_back(frame, event.type);
        }
    }
    // sleep 20 starts at frame 1 (0 ms) and ends at frame 3 (32 ms), where the click begins. The
    // key starts in frame 5 with the button up, and its down waits for the next frame.
    const std::vector<std::pair<std::uint64_t, WindowEventType>> expected = {
        {3, WindowEventType::kMouseMove},
        {4, WindowEventType::kMouseButtonDown},
        {5, WindowEventType::kMouseButtonUp},
        {6, WindowEventType::kKeyDown},
        {7, WindowEventType::kKeyUp},
    };
    CHECK(seen == expected);
    CHECK(window.Frames() == 8);

    // The sink saw the same events, in the same order, stamped with the clock.
    REQUIRE(input.events.size() == 5);
    CHECK(input.events[0].type == InputEventType::kMouseMove);
    CHECK(input.events[0].x == 157);
    CHECK(input.events[0].y == 172);
    CHECK(input.events[0].timeMs == 32);
    CHECK(input.events[1].type == InputEventType::kMouseButtonDown);
    CHECK(input.events[1].timeMs == 48);
    CHECK(input.events[2].type == InputEventType::kMouseButtonUp);
    CHECK(input.events[2].timeMs == 64);
    CHECK(input.events[3].type == InputEventType::kKeyDown);
    CHECK(input.events[3].key == 0x28);
    CHECK(input.events[3].timeMs == 80);
    CHECK(input.events[4].type == InputEventType::kKeyUp);
    CHECK(input.events[4].timeMs == 96);
    CHECK(player.Finished());
}

TEST_CASE("a scripted event reaches the queue and the sink; a real event in the same frame keeps "
          "its place") {
    WindowMock inner;
    inner.stateResult = focusedState();
    InputMock input;
    ClockMock clock;
    ScriptPlayer player = playerFor("click 10 20\n");
    ScriptedWindow window(inner, player, clock, {});
    window.BindToCallingThread();
    REQUIRE(window.AttachInput(input).Ok());

    inner.pendingEvents.push_back(realKey(WindowEventType::kKeyDown, 0x04));
    const std::vector<WindowEvent> events = pumpTurn(window);
    REQUIRE(events.size() == 2);
    CHECK(events[0].type == WindowEventType::kKeyDown); // the real one first
    CHECK(events[0].input.key == 0x04);
    CHECK(events[1].type == WindowEventType::kMouseMove);
    REQUIRE(input.events.size() == 2);
    CHECK(input.events[0].type == InputEventType::kKeyDown);
    CHECK(input.events[1].type == InputEventType::kMouseMove);
    CHECK(input.events[1].x == 10);
}

TEST_CASE("only the bound thread pumps: another thread delivers nothing and advances no frame") {
    WindowMock inner;
    inner.stateResult = focusedState();
    InputMock input;
    ClockMock clock;
    ScriptPlayer player = playerFor("click 10 20\n");
    ScriptedWindow window(inner, player, clock, {});
    window.BindToCallingThread();
    REQUIRE(window.AttachInput(input).Ok());

    Error fromOther = Error::kNone;
    std::thread other([&] { fromOther = window.PumpEvents().Code(); });
    other.join();
    CHECK(fromOther == Error::kWrongState);
    CHECK(window.Frames() == 0);
    CHECK(input.events.empty());
    CHECK(inner.log.Count("PumpEvents") == 0);

    CHECK(pumpTurn(window).size() == 1);
    CHECK(input.events.size() == 1);
}

TEST_CASE("an unbound window pumps nothing") {
    WindowMock inner;
    ClockMock clock;
    ScriptPlayer player = playerFor("click 10 20\n");
    ScriptedWindow window(inner, player, clock, {});
    CHECK(window.PumpEvents().Code() == Error::kWrongState);
    CHECK(window.Frames() == 0);
}

TEST_CASE("a window without the system's focus is given it before a scripted click") {
    WindowMock inner;
    WindowState state = focusedState();
    state.focused = false;
    inner.stateResult = state;
    InputMock input;
    ClockMock clock;
    ScriptPlayer player = playerFor("click 10 20\n");
    ScriptedWindow window(inner, player, clock, {});
    window.BindToCallingThread();
    REQUIRE(window.AttachInput(input).Ok());

    const std::vector<WindowEvent> events = pumpTurn(window);
    REQUIRE(events.size() == 2);
    CHECK(events[0].type == WindowEventType::kFocusGained);
    CHECK(events[1].type == WindowEventType::kMouseMove);
    CHECK(input.log.Count("OnFocusChanged") == 1);
    REQUIRE(input.events.size() == 1);

    // The down and the up need no focus of their own.
    CHECK(pumpTurn(window).size() == 1);
    CHECK(pumpTurn(window).size() == 1);
}

TEST_CASE("headless: over the stub window the script still plays and its sink still hears it") {
    std::unique_ptr<sfcop::platform::Window> stub = sfcop::platform::makeWindowStub();
    InputMock input;
    ClockMock clock;
    ScriptPlayer player = playerFor("click 3 4\n");
    ScriptedWindow window(*stub, player, clock, {});
    window.BindToCallingThread();
    CHECK(window.AttachInput(input).Code() == Error::kUnsupported); // the stub's answer
    const std::vector<WindowEvent> first = pumpTurn(window);
    REQUIRE(first.size() == 1); // no focus event: the stub has no state
    CHECK(first[0].type == WindowEventType::kMouseMove);
    CHECK(pumpTurn(window).size() == 1);
    CHECK(pumpTurn(window).size() == 1);
    REQUIRE(input.events.size() == 3);
    CHECK(input.events[2].type == InputEventType::kMouseButtonUp);
    CHECK(window.GetState().Code() == Error::kUnsupported);
}

TEST_CASE("a wait that runs out is reported once, naming its line") {
    WindowMock inner;
    ClockMock clock;
    ScriptPlayer player = playerFor("# wait\nwaitmark 1 Setup\n");
    std::vector<PlayerFailure> failures;
    ScriptedWindow window(inner, player, clock,
                          [&failures](const PlayerFailure &failure) { failures.push_back(failure); });
    window.BindToCallingThread();
    clock.ticksResult = 0;
    REQUIRE(window.PumpEvents().Ok());
    clock.ticksResult = 1000;
    REQUIRE(window.PumpEvents().Ok());
    clock.ticksResult = 2000;
    REQUIRE(window.PumpEvents().Ok());
    REQUIRE(failures.size() == 1);
    CHECK(failures[0].line == 2);
    CHECK(failures[0].message == "waited 1 s for the mark Setup; it did not come");
}

TEST_CASE("every other call goes to the inner window unchanged") {
    WindowMock inner;
    inner.setTitleResult = Error::kWrongState;
    ClockMock clock;
    ScriptPlayer player = playerFor("");
    ScriptedWindow window(inner, player, clock, {});
    CHECK(window.SetTitle("Test").Code() == Error::kWrongState);
    CHECK(window.Create(sfcop::platform::WindowDesc{}).Ok());
    CHECK(window.SetFrameCap(sfcop::platform::FrameCap{30}).Ok());
    CHECK(window.GetDesktopMode().Value().refreshHz == 60);
    CHECK(inner.log.Count("SetTitle") == 1);
    CHECK(inner.log.Count("Create") == 1);
    CHECK(inner.log.Count("SetFrameCap") == 1);
}
