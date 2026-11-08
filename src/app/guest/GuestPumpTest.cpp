// The event pump through the Window mock: events are read on the bound thread only.
#include "app/guest/GuestPump.h"
#include "hle/user32/User32TestRig.h"
#include "platform/Window/WindowMock.h"

#include <doctest/doctest.h>

#include <thread>
#include <vector>

namespace sfcop::guest {

TEST_CASE("the pump reads the window's events on the thread that bound it") {
    platform::WindowMock window;
    int closes = 0;
    EventPump pump(window, [&](const platform::WindowEvent &event) {
        closes += event.type == platform::WindowEventType::kCloseRequested ? 1 : 0;
    });
    pump.BindToCallingThread();
    platform::WindowEvent close;
    close.type = platform::WindowEventType::kCloseRequested;
    window.pendingEvents.push_back(close);

    CHECK(pump.Pump());
    CHECK(window.log.Count("PumpEvents") == 1);
    CHECK(closes == 1);
}

TEST_CASE("the pump hands every event on, in order") {
    platform::WindowMock window;
    std::vector<platform::WindowEventType> seen;
    EventPump pump(window, [&](const platform::WindowEvent &event) { seen.push_back(event.type); });
    pump.BindToCallingThread();
    for (const auto type : {platform::WindowEventType::kFocusGained,
                            platform::WindowEventType::kKeyDown,
                            platform::WindowEventType::kCloseRequested}) {
        platform::WindowEvent event;
        event.type = type;
        window.pendingEvents.push_back(event);
    }

    CHECK(pump.Pump());
    REQUIRE(seen.size() == 3);
    CHECK(seen[0] == platform::WindowEventType::kFocusGained);
    CHECK(seen[1] == platform::WindowEventType::kKeyDown);
    CHECK(seen[2] == platform::WindowEventType::kCloseRequested);
    CHECK(window.pendingEvents.empty());
}

TEST_CASE("the pump does nothing on another thread") {
    platform::WindowMock window;
    int closes = 0;
    EventPump pump(window, [&](const platform::WindowEvent &) { ++closes; });
    pump.BindToCallingThread();
    platform::WindowEvent close;
    close.type = platform::WindowEventType::kCloseRequested;
    window.pendingEvents.push_back(close);

    bool pumped = true;
    std::thread other([&] { pumped = pump.Pump(); });
    other.join();

    CHECK_FALSE(pumped);
    CHECK(window.log.Count("PumpEvents") == 0);
    CHECK(window.log.Count("PollEvent") == 0);
    CHECK(closes == 0);
    CHECK(window.pendingEvents.size() == 1); // still waiting for the main thread
}

TEST_CASE("an unbound pump does nothing") {
    platform::WindowMock window;
    EventPump pump(window, nullptr);
    CHECK_FALSE(pump.Pump());
    CHECK(window.log.Count("PumpEvents") == 0);
}

TEST_CASE("a turn that read no event ends a frame, one that read an event does not, and another thread's call changes nothing") {
    platform::WindowMock window;
    EventPump pump(window, [](const platform::WindowEvent &) {});
    CHECK_FALSE(pump.FrameEnded()); // no turn yet
    pump.BindToCallingThread();
    CHECK(pump.Pump());
    CHECK(pump.FrameEnded());
    platform::WindowEvent focus;
    focus.type = platform::WindowEventType::kFocusGained;
    window.pendingEvents.push_back(focus);
    CHECK(pump.Pump());
    CHECK_FALSE(pump.FrameEnded());
    CHECK(pump.Pump());
    CHECK(pump.FrameEnded());
    bool pumped = true;
    std::thread other([&] { pumped = pump.Pump(); });
    other.join();
    CHECK_FALSE(pumped);
    CHECK(pump.FrameEnded());
}

} // namespace sfcop::guest

// ---- the turns inside the game's calls (X7) ----

namespace sfcop::guest {

namespace {

platform::WindowEvent eventOf(platform::WindowEventType type) {
    platform::WindowEvent event;
    event.type = type;
    return event;
}

} // namespace

TEST_CASE("a turn inside a call reads the window once its interval has passed since the last turn") {
    platform::WindowMock window;
    std::uint64_t now = 1000;
    EventPump pump(window, [](const platform::WindowEvent &) {}, [&] { return now; });
    pump.BindToCallingThread();
    pump.EnableInsideCalls(50);
    now = 1049;
    CHECK_FALSE(pump.PumpInsideCall()); // 49 ms since the bind
    now = 1050;
    CHECK(pump.PumpInsideCall());
    CHECK(window.log.Count("PumpEvents") == 1);
    now = 1080;
    CHECK_FALSE(pump.PumpInsideCall());
    now = 1090;
    CHECK(pump.Pump()); // a message turn always reads, and restarts the interval
    CHECK(window.log.Count("PumpEvents") == 2);
    now = 1139;
    CHECK_FALSE(pump.PumpInsideCall());
    now = 1140;
    CHECK(pump.PumpInsideCall());
    CHECK(window.log.Count("PumpEvents") == 3);
    CHECK(pump.Totals().messageTurns == 1);
    CHECK(pump.Totals().insideCallTurns == 2);
}

TEST_CASE("turns inside calls are off until enabled") {
    platform::WindowMock window;
    std::uint64_t now = 0;
    EventPump pump(window, [](const platform::WindowEvent &) {}, [&] { return now; });
    pump.BindToCallingThread();
    window.pendingEvents.push_back(eventOf(platform::WindowEventType::kKeyDown));
    now = 5000;
    CHECK_FALSE(pump.PumpInsideCall());
    CHECK(window.log.Count("PumpEvents") == 0);
    CHECK(window.pendingEvents.size() == 1);
}

TEST_CASE("a turn inside a call on another thread does nothing") {
    platform::WindowMock window;
    std::uint64_t now = 0;
    int seen = 0;
    EventPump pump(window, [&](const platform::WindowEvent &) { ++seen; }, [&] { return now; });
    pump.BindToCallingThread();
    pump.EnableInsideCalls(50);
    window.pendingEvents.push_back(eventOf(platform::WindowEventType::kCloseRequested));
    now = 500;
    bool pumped = true;
    std::thread other([&] { pumped = pump.PumpInsideCall(); });
    other.join();
    CHECK_FALSE(pumped);
    CHECK(window.log.Count("PumpEvents") == 0);
    CHECK(window.log.Count("PollEvent") == 0);
    CHECK(seen == 0);
    CHECK(window.pendingEvents.size() == 1);
    CHECK(pump.Totals().insideCallTurns == 0);
}

TEST_CASE("a turn inside a call is never a frame, and the frames come out as without it") {
    // Without turns inside calls: message turn, (focus arrives) message turn, message turn.
    {
        platform::WindowMock window;
        std::uint64_t now = 0;
        EventPump pump(window, [](const platform::WindowEvent &) {}, [&] { return now; });
        pump.BindToCallingThread();
        std::vector<bool> frames;
        now = 16;
        pump.Pump();
        frames.push_back(pump.FrameEnded());
        window.pendingEvents.push_back(eventOf(platform::WindowEventType::kFocusGained));
        now = 3000; // a load
        pump.Pump();
        frames.push_back(pump.FrameEnded());
        now = 3016;
        pump.Pump();
        frames.push_back(pump.FrameEnded());
        CHECK(frames == std::vector<bool>{true, false, true});
    }
    // With them: the focus is read inside the load; the frames are the same.
    {
        platform::WindowMock window;
        std::uint64_t now = 0;
        EventPump pump(window, [](const platform::WindowEvent &) {}, [&] { return now; });
        pump.BindToCallingThread();
        pump.EnableInsideCalls(50);
        std::vector<bool> frames;
        now = 16;
        pump.Pump();
        frames.push_back(pump.FrameEnded());
        window.pendingEvents.push_back(eventOf(platform::WindowEventType::kFocusGained));
        now = 100;
        CHECK(pump.PumpInsideCall());
        CHECK(window.pendingEvents.empty());
        CHECK(pump.FrameEnded()); // unchanged by the turn inside the call
        now = 150;
        CHECK(pump.PumpInsideCall()); // reads nothing
        now = 3000;
        pump.Pump(); // reads nothing, but the load's turns read the focus
        frames.push_back(pump.FrameEnded());
        now = 3016;
        pump.Pump();
        frames.push_back(pump.FrameEnded());
        CHECK(frames == std::vector<bool>{true, false, true});
        CHECK(pump.Totals().eventsInsideCalls == 1);
    }
}

TEST_CASE("each gap between two turns of the bound thread is measured, and the long ones reported") {
    platform::WindowMock window;
    std::uint64_t now = 1000;
    EventPump pump(window, [](const platform::WindowEvent &) {}, [&] { return now; });
    std::vector<EventPump::Gap> gaps;
    pump.SetGapTrace(100, [&](const EventPump::Gap &gap) { gaps.push_back(gap); });
    pump.BindToCallingThread();
    pump.EnableInsideCalls(50);
    now = 1016;
    pump.Pump(); // 16 ms: under the threshold
    now = 1300;
    pump.PumpInsideCall(); // 284 ms
    now = 1350;
    pump.PumpInsideCall(); // 50 ms
    now = 2350;
    pump.Pump(); // 1000 ms
    REQUIRE(gaps.size() == 2);
    CHECK(gaps[0].endMs == 300);
    CHECK(gaps[0].lengthMs == 284);
    CHECK(gaps[0].insideCall);
    CHECK(gaps[1].endMs == 1350);
    CHECK(gaps[1].lengthMs == 1000);
    CHECK_FALSE(gaps[1].insideCall);
    CHECK(pump.Totals().longest.lengthMs == 1000);
    CHECK(pump.Totals().longest.endMs == 1350);
}

} // namespace sfcop::guest

// ---- with the user32 module's queue: what the game reads ----

namespace sfcop::hle::user32 {

TEST_CASE("events read inside a load reach the game's queue in order and come out when it peeks") {
    test::Rig rig;
    std::uint64_t now = 0;
    guest::EventPump pump(rig.window, [&](const platform::WindowEvent &event) {
        rig.user.OnWindowEvent(event);
    }, [&] { return now; });
    pump.BindToCallingThread();
    pump.EnableInsideCalls(50);
    rig.user.SetHostPump([&] { pump.Pump(); }); // the game's PeekMessageA pumps through it
    const std::uint32_t hwnd = rig.GameWindow();
    rig.Drain();
    const std::size_t guestCalls = rig.guest.calls.size();

    // The load: the game calls its file rows and never peeks.
    rig.Key(0x04, true); // A down
    now = 50;
    rig.now = 1050;
    CHECK(pump.PumpInsideCall());
    CHECK(rig.user.Queue().Size() == 1); // queued, not delivered
    CHECK(rig.guest.calls.size() == guestCalls); // a turn inside a call never calls the game

    // The game peeks once in the middle of the load: it gets A down; the peek's own pump reads
    // what came since (A up, B down) behind it.
    rig.Key(0x04, false);
    rig.Key(0x05, true);
    now = 70;
    rig.now = 1070;
    const Retrieved mid = rig.user.Peek(0, 0, 0, true);
    REQUIRE(mid.found);
    CHECK(mid.message.message == kWmKeyDown);
    CHECK(mid.message.wParam == 'A');
    CHECK(mid.message.time == 1050);
    CHECK(rig.user.Queue().Size() == 2);

    // More of the load: B up and the close box.
    rig.Key(0x05, false);
    rig.Event([] {
        platform::WindowEvent close;
        close.type = platform::WindowEventType::kCloseRequested;
        return close;
    }());
    now = 100;
    rig.now = 1100;
    CHECK_FALSE(pump.PumpInsideCall()); // 30 ms since the peek's turn
    now = 120;
    rig.now = 1120;
    CHECK(pump.PumpInsideCall());
    CHECK(rig.user.Queue().Size() == 4);
    CHECK(rig.guest.calls.size() == guestCalls);

    // The load ends; the game peeks until its queue is empty.
    now = 3000;
    rig.now = 4000;
    const std::vector<Message> got = rig.Drain();
    REQUIRE(got.size() == 4);
    CHECK(got[0].message == kWmKeyUp);
    CHECK(got[0].wParam == 'A');
    CHECK(got[0].time == 1070);
    CHECK(got[1].message == kWmKeyDown);
    CHECK(got[1].wParam == 'B');
    CHECK(got[1].time == 1070);
    CHECK(got[2].message == kWmKeyUp);
    CHECK(got[2].wParam == 'B');
    CHECK(got[2].time == 1120);
    CHECK(got[3].message == kWmClose);
    CHECK(got[3].hwnd == hwnd);
    CHECK(got[3].time == 1120);
}

} // namespace sfcop::hle::user32
