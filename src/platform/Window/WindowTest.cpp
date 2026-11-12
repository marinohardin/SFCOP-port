#include "platform/Window/Window.h"
#include "platform/Window/WindowMock.h"
#include "platform/Input/InputMock.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace sfcop::platform {
namespace {

WindowEvent event(WindowEventType type) {
    WindowEvent made;
    made.type = type;
    return made;
}

// A caller written against the interface only, as the app's frame loop is: pumps once, drains
// the queue, and reports whether the user asked to close.
bool frameWantsClose(Window &window, std::vector<WindowEventType> &seen) {
    if (!window.PumpEvents().Ok()) {
        return false;
    }
    bool close = false;
    for (;;) {
        Result<std::optional<WindowEvent>> next = window.PollEvent();
        if (!next.Ok() || !next.Value().has_value()) {
            return close;
        }
        seen.push_back(next.Value()->type);
        close = close || next.Value()->type == WindowEventType::kCloseRequested;
    }
}

} // namespace

TEST_CASE("a frame loop sees pumped window events in order and stops on a close request") {
    WindowMock mock;
    mock.pendingEvents.push_back(event(WindowEventType::kFocusGained));
    mock.pendingEvents.push_back(event(WindowEventType::kCloseRequested));
    std::vector<WindowEventType> seen;
    CHECK(frameWantsClose(mock, seen));
    CHECK(seen == std::vector<WindowEventType>{WindowEventType::kFocusGained,
                                               WindowEventType::kCloseRequested});
    seen.clear();
    CHECK_FALSE(frameWantsClose(mock, seen));
    CHECK(seen.empty());
    CHECK(mock.log.Count("PumpEvents") == 2);
}

TEST_CASE("events wait in the operating system until the window is pumped") {
    WindowMock mock;
    Window &window = mock;
    mock.pendingEvents.push_back(event(WindowEventType::kResized));
    CHECK_FALSE(window.PollEvent().Value().has_value());
    mock.pumpEventsResult = Error::kWrongState;
    std::vector<WindowEventType> seen;
    CHECK_FALSE(frameWantsClose(mock, seen));
    CHECK(seen.empty());
    CHECK(mock.pendingEvents.size() == 1);
}

TEST_CASE("the window mock records creation and mode changes with their arguments") {
    WindowMock mock;
    Window &window = mock;
    WindowDesc desc;
    desc.title = "Test Window";
    desc.width = 800;
    desc.height = 600;
    CHECK(window.Create(desc).Ok());
    CHECK(window.SetMode(WindowMode::kFullscreen, 1024, 768).Ok());
    CHECK(window.SetFrameCap(FrameCap{60}).Ok());
    CHECK(window.GetDesktopMode().Value().refreshHz == 60);
    CHECK(window.SetWholeScale(true).Ok());
    CHECK(window.GetWholeScale().Value() == false); // what the test programmed
    CHECK(mock.lastDesc.resizable);
    CHECK(mock.log.Calls() == std::vector<std::string>{
                                  "Create(\"Test Window\", 0, 800, 600)",
                                  "SetMode(1, 1024, 768)",
                                  "SetFrameCap(60)",
                                  "GetDesktopMode()",
                                  "SetWholeScale(true)",
                                  "GetWholeScale()",
                              });
}

TEST_CASE("an input attached to a window receives its key, mouse and focus events when pumped") {
    WindowMock window;
    InputMock input;
    REQUIRE(window.AttachInput(input).Ok());
    input.keyboard[0x04] = true;
    REQUIRE(input.SetRelativeMode(true).Ok());

    WindowEvent key = event(WindowEventType::kKeyDown);
    key.input.type = InputEventType::kKeyDown;
    key.input.key = 0x29;
    key.input.timeMs = 500;
    WindowEvent click = event(WindowEventType::kMouseButtonDown);
    click.input.type = InputEventType::kMouseButtonDown;
    click.input.timeMs = 510;
    window.pendingEvents.push_back(key);
    window.pendingEvents.push_back(event(WindowEventType::kResized));
    window.pendingEvents.push_back(click);
    window.pendingEvents.push_back(event(WindowEventType::kFocusLost));
    REQUIRE(window.PumpEvents().Ok());

    CHECK(window.relativeMouse);
    CHECK(input.log.Calls() == std::vector<std::string>{
                                   "SetRelativeMode(true)",
                                   "OnInputEvent(0, 41, 500)",
                                   "OnInputEvent(3, 0, 510)",
                                   "OnFocusChanged(false)",
                               });
    REQUIRE(input.events.size() == 2);
    CHECK(input.events[0].sequence == 1);
    CHECK(input.events[1].sequence == 2);
    CHECK_FALSE(input.keyboard[0x04]);
    CHECK(window.queuedEvents.size() == 4);

    REQUIRE(window.DetachInput().Ok());
    window.pendingEvents.push_back(key);
    REQUIRE(window.PumpEvents().Ok());
    CHECK(input.events.size() == 2);
    CHECK_FALSE(window.relativeMouse);
}

TEST_CASE("every method of the window stub reports unsupported") {
    std::unique_ptr<Window> window = makeWindowStub();
    CHECK(window->Create(WindowDesc{}).Code() == Error::kUnsupported);
    CHECK(window->Destroy().Code() == Error::kUnsupported);
    CHECK(window->SetMode(WindowMode::kWindowed, 640, 480).Code() == Error::kUnsupported);
    CHECK(window->GetState().Code() == Error::kUnsupported);
    CHECK(window->GetDesktopMode().Code() == Error::kUnsupported);
    CHECK(window->ListDisplayModes().Code() == Error::kUnsupported);
    CHECK(window->SetTitle("Test").Code() == Error::kUnsupported);
    CHECK(window->SetCursorVisible(false).Code() == Error::kUnsupported);
    CHECK(window->SetFrameCap(FrameCap{}).Code() == Error::kUnsupported);
    CHECK(window->GetFrameCap().Code() == Error::kUnsupported);
    CHECK(window->SetWholeScale(true).Code() == Error::kUnsupported);
    CHECK(window->GetWholeScale().Code() == Error::kUnsupported);
    InputMock input;
    CHECK(window->AttachInput(input).Code() == Error::kUnsupported);
    CHECK(window->DetachInput().Code() == Error::kUnsupported);
    CHECK(window->PumpEvents().Code() == Error::kUnsupported);
    CHECK(window->PollEvent().Code() == Error::kUnsupported);
}

} // namespace sfcop::platform
