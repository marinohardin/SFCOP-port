// The event pump through the Window mock: events are read on the bound thread only.
#include "app/guest/GuestPump.h"
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
