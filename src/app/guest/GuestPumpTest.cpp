// The event pump through the Window mock: events are read on the bound thread only.
#include "app/guest/GuestPump.h"
#include "platform/Window/WindowMock.h"

#include <doctest/doctest.h>

#include <thread>

namespace sfcop::guest {

TEST_CASE("the pump reads the window's events on the thread that bound it") {
    platform::WindowMock window;
    int closes = 0;
    EventPump pump(window, [&] { ++closes; });
    pump.BindToCallingThread();
    platform::WindowEvent close;
    close.type = platform::WindowEventType::kCloseRequested;
    window.pendingEvents.push_back(close);

    CHECK(pump.Pump());
    CHECK(window.log.Count("PumpEvents") == 1);
    CHECK(closes == 1);
}

TEST_CASE("the pump does nothing on another thread") {
    platform::WindowMock window;
    int closes = 0;
    EventPump pump(window, [&] { ++closes; });
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
    EventPump pump(window, {});
    CHECK_FALSE(pump.Pump());
    CHECK(window.log.Count("PumpEvents") == 0);
}

} // namespace sfcop::guest
