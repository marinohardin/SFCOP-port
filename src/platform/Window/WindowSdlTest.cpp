// The SDL3 Window backend through the Window interface, on SDL's dummy video driver so that it
// runs without a display. Operating-system events are synthesised with SDL_PushEvent().
#include "platform/Window/Window.h"

#include <SDL3/SDL.h>
#include <doctest/doctest.h>

#include <utility>
#include <vector>

namespace sfcop::platform {

// Defined in WindowSdl.cpp for this test: the next mode change fails.
void failNextWindowModeForTest();

namespace {

// Selects the dummy video driver and a hidden window before the backend initialises SDL.
struct DummyVideo {
    DummyVideo() {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
        SDL_setenv_unsafe("SFCOP_WINDOW_HIDDEN", "1", 1);
    }
    ~DummyVideo() { SDL_ResetHint(SDL_HINT_VIDEO_DRIVER); }
};

class RecordingSink final : public InputSink {
public:
    std::vector<InputEvent> events;
    std::vector<bool> focus;
    bool relative = false;

    void OnInputEvent(const InputEvent &event) override { events.push_back(event); }
    void OnFocusChanged(bool focused) override { focus.push_back(focused); }
    bool WantsRelativeMouse() const override { return relative; }
};

WindowDesc smallWindow() {
    WindowDesc desc;
    desc.title = "TestCaptain's window";
    desc.width = 640;
    desc.height = 480;
    return desc;
}

SDL_WindowID onlyWindowId() {
    int count = 0;
    SDL_Window **windows = SDL_GetWindows(&count);
    SDL_WindowID id = 0;
    if (windows != nullptr && count == 1) {
        id = SDL_GetWindowID(windows[0]);
    }
    SDL_free(windows);
    return id;
}

std::vector<WindowEvent> drain(Window &window) {
    std::vector<WindowEvent> out;
    for (;;) {
        auto polled = window.PollEvent();
        REQUIRE(polled.Ok());
        if (!polled.Value().has_value()) {
            return out;
        }
        out.push_back(*polled.Value());
    }
}

void pushKey(SDL_WindowID id, Uint32 type, SDL_Scancode code, Uint64 timestampNs) {
    SDL_Event event;
    SDL_zero(event);
    event.type = type;
    event.key.timestamp = timestampNs;
    event.key.windowID = id;
    event.key.scancode = code;
    event.key.down = type == SDL_EVENT_KEY_DOWN;
    REQUIRE(SDL_PushEvent(&event));
}

void pushMotion(SDL_WindowID id, float x, float y, float dx, float dy) {
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.timestamp = 1000000;
    event.motion.windowID = id;
    event.motion.x = x;
    event.motion.y = y;
    event.motion.xrel = dx;
    event.motion.yrel = dy;
    REQUIRE(SDL_PushEvent(&event));
}

void pushButton(SDL_WindowID id, bool down, float x, float y) {
    SDL_Event event;
    SDL_zero(event);
    event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.timestamp = 1000000;
    event.button.windowID = id;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.down = down;
    event.button.clicks = 1;
    event.button.x = x;
    event.button.y = y;
    REQUIRE(SDL_PushEvent(&event));
}

// The frame's size in points, as SDL has it.
std::pair<int, int> frameSize(SDL_WindowID id) {
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(SDL_GetWindowFromID(id), &width, &height);
    return {width, height};
}

// Where each pushed button-down landed, in logical pixels.
std::vector<std::pair<std::int32_t, std::int32_t>>
clicksAt(Window &window, SDL_WindowID id, const std::vector<std::pair<float, float>> &points) {
    RecordingSink sink;
    REQUIRE(window.AttachInput(sink).Ok());
    for (const auto &point : points) {
        pushButton(id, true, point.first, point.second);
    }
    REQUIRE(window.PumpEvents().Ok());
    REQUIRE(window.DetachInput().Ok());
    std::vector<std::pair<std::int32_t, std::int32_t>> out;
    for (const InputEvent &event : sink.events) {
        if (event.type == InputEventType::kMouseButtonDown) {
            out.emplace_back(event.x, event.y);
        }
    }
    return out;
}

using Points = std::vector<std::pair<std::int32_t, std::int32_t>>;

void pushWindowEvent(SDL_WindowID id, Uint32 type) {
    SDL_Event event;
    SDL_zero(event);
    event.type = type;
    event.window.timestamp = 2000000;
    event.window.windowID = id;
    REQUIRE(SDL_PushEvent(&event));
}

} // namespace

TEST_CASE("an SDL window opens at the requested logical size and closes") {
    DummyVideo video;
    auto window = makeWindow();
    REQUIRE(window->Create(smallWindow()).Ok());
    auto state = window->GetState();
    REQUIRE(state.Ok());
    CHECK(state.Value().mode == WindowMode::kWindowed);
    CHECK(state.Value().width == 640);
    CHECK(state.Value().height == 480);
    CHECK(state.Value().drawableWidth >= 640);
    CHECK(state.Value().drawableHeight >= 480);
    CHECK(window->Destroy().Ok());
    CHECK(window->Destroy().Ok()); // closing a closed window succeeds
    CHECK(window->GetState().Code() == Error::kWrongState);
}

TEST_CASE("an SDL window refuses a second open and sizes out of range") {
    DummyVideo video;
    auto window = makeWindow();
    WindowDesc desc = smallWindow();
    desc.width = 0;
    CHECK(window->Create(desc).Code() == Error::kInvalidArgument);
    desc.width = 16385;
    CHECK(window->Create(desc).Code() == Error::kInvalidArgument);
    desc.width = 640;
    desc.height = 0;
    CHECK(window->Create(desc).Code() == Error::kInvalidArgument);
    REQUIRE(window->Create(smallWindow()).Ok());
    CHECK(window->Create(smallWindow()).Code() == Error::kWrongState);
}

TEST_CASE("calls that need an open SDL window fail with WrongState before it opens") {
    DummyVideo video;
    auto window = makeWindow();
    CHECK(window->SetMode(WindowMode::kWindowed, 800, 600).Code() == Error::kWrongState);
    CHECK(window->GetState().Code() == Error::kWrongState);
    CHECK(window->SetTitle("TestCaptain").Code() == Error::kWrongState);
    CHECK(window->SetCursorVisible(false).Code() == Error::kWrongState);
}

TEST_CASE("SetMode changes the logical size and reports it as kResized at the next pump") {
    DummyVideo video;
    auto window = makeWindow();
    REQUIRE(window->Create(smallWindow()).Ok());
    REQUIRE(window->PumpEvents().Ok());
    drain(*window);

    CHECK(window->SetMode(WindowMode::kWindowed, 0, 600).Code() == Error::kInvalidArgument);
    CHECK(window->SetMode(WindowMode::kWindowed, 800, 16385).Code() == Error::kInvalidArgument);
    REQUIRE(window->SetMode(WindowMode::kWindowed, 800, 600).Ok());
    CHECK(window->GetState().Value().width == 800);
    CHECK(window->GetState().Value().height == 600);
    REQUIRE(window->PumpEvents().Ok());
    bool resized = false;
    for (const WindowEvent &event : drain(*window)) {
        if (event.type == WindowEventType::kResized) {
            CHECK(event.width == 800);
            CHECK(event.height == 600);
            resized = true;
        }
    }
    CHECK(resized);
}

TEST_CASE("fullscreen on the dummy driver either switches or says it is unsupported") {
    DummyVideo video;
    auto window = makeWindow();
    REQUIRE(window->Create(smallWindow()).Ok());
    const Status status = window->SetMode(WindowMode::kFullscreen, 640, 480);
    if (status.Ok()) {
        CHECK(window->GetState().Value().mode == WindowMode::kFullscreen);
        CHECK(window->GetState().Value().width == 640);
        REQUIRE(window->SetMode(WindowMode::kWindowed, 640, 480).Ok());
        CHECK(window->GetState().Value().mode == WindowMode::kWindowed);
    } else {
        CHECK(status.Code() == Error::kUnsupported);
    }
}

TEST_CASE("the desktop mode is the dummy display's, before and after the window opens") {
    DummyVideo video;
    auto window = makeWindow();
    auto before = window->GetDesktopMode();
    REQUIRE(before.Ok());
    CHECK(before.Value().width == 1024);
    CHECK(before.Value().height == 768);
    CHECK(before.Value().refreshHz == 0); // the dummy display does not say
    REQUIRE(window->Create(smallWindow()).Ok());
    auto after = window->GetDesktopMode();
    REQUIRE(after.Ok());
    CHECK(after.Value().width == 1024);
    CHECK(after.Value().height == 768);
}

TEST_CASE("the display mode list is largest first with no duplicates") {
    DummyVideo video;
    auto window = makeWindow();
    auto modes = window->ListDisplayModes();
    REQUIRE(modes.Ok());
    const std::vector<DisplayMode> &list = modes.Value();
    for (std::size_t i = 1; i < list.size(); ++i) {
        const std::uint64_t before = std::uint64_t{list[i - 1].width} * list[i - 1].height;
        const std::uint64_t here = std::uint64_t{list[i].width} * list[i].height;
        CHECK(before >= here);
        const bool same = list[i - 1].width == list[i].width &&
                          list[i - 1].height == list[i].height &&
                          list[i - 1].refreshHz == list[i].refreshHz;
        CHECK_FALSE(same);
    }
}

TEST_CASE("title and cursor visibility are accepted on an open SDL window") {
    DummyVideo video;
    auto window = makeWindow();
    REQUIRE(window->Create(smallWindow()).Ok());
    CHECK(window->SetTitle("Orion Pirates - TestCaptain").Ok());
    CHECK(window->SetCursorVisible(false).Ok());
    CHECK(window->SetCursorVisible(true).Ok());
}

TEST_CASE("the frame cap defaults to the display rate and refuses over 1000") {
    DummyVideo video;
    auto window = makeWindow();
    REQUIRE(window->GetFrameCap().Ok());
    CHECK(window->GetFrameCap().Value().maxFramesPerSecond == 0);
    CHECK(window->SetFrameCap(FrameCap{1001}).Code() == Error::kInvalidArgument);
    CHECK(window->SetFrameCap(FrameCap{1000}).Ok());
    CHECK(window->SetFrameCap(FrameCap{60}).Ok());
    CHECK(window->GetFrameCap().Value().maxFramesPerSecond == 60);
}

TEST_CASE("PumpEvents delivers a synthesised key to the sink and to the window queue") {
    DummyVideo video;
    auto window = makeWindow();
    REQUIRE(window->Create(smallWindow()).Ok());
    REQUIRE(window->PumpEvents().Ok());
    drain(*window);
    RecordingSink sink;
    REQUIRE(window->AttachInput(sink).Ok());
    const SDL_WindowID id = onlyWindowId();
    REQUIRE(id != 0);

    pushKey(id, SDL_EVENT_KEY_DOWN, SDL_SCANCODE_A, 7000000000ull);
    pushKey(id, SDL_EVENT_KEY_UP, SDL_SCANCODE_RETURN, 7005000000ull);
    REQUIRE(window->PumpEvents().Ok());

    REQUIRE(sink.events.size() == 2);
    CHECK(sink.events[0].type == InputEventType::kKeyDown);
    CHECK(sink.events[0].key == 0x04); // HID usage of A
    CHECK(sink.events[0].timeMs == 7000);
    CHECK(sink.events[1].type == InputEventType::kKeyUp);
    CHECK(sink.events[1].key == 0x28); // HID usage of Return
    CHECK(sink.events[1].timeMs == 7005);

    const std::vector<WindowEvent> queued = drain(*window);
    REQUIRE(queued.size() == 2);
    CHECK(queued[0].type == WindowEventType::kKeyDown);
    CHECK(queued[0].input.key == 0x04);
    CHECK(queued[0].timeMs == 7000);
    CHECK(queued[1].type == WindowEventType::kKeyUp);
    CHECK(queued[1].input.key == 0x28);
}

TEST_CASE("mouse motion, buttons and wheel arrive in logical pixels") {
    DummyVideo video;
    auto window = makeWindow();
    REQUIRE(window->Create(smallWindow()).Ok());
    REQUIRE(window->PumpEvents().Ok());
    drain(*window);
    RecordingSink sink;
    REQUIRE(window->AttachInput(sink).Ok());
    const SDL_WindowID id = onlyWindowId();

    pushMotion(id, 100.0f, 50.0f, 3.0f, -2.0f);
    SDL_Event button;
    SDL_zero(button);
    button.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    button.button.windowID = id;
    button.button.button = SDL_BUTTON_RIGHT;
    button.button.down = true;
    button.button.x = 10.0f;
    button.button.y = 20.0f;
    REQUIRE(SDL_PushEvent(&button));
    SDL_Event wheel;
    SDL_zero(wheel);
    wheel.type = SDL_EVENT_MOUSE_WHEEL;
    wheel.wheel.windowID = id;
    wheel.wheel.y = 1.0f;
    wheel.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    wheel.wheel.mouse_x = 30.0f;
    wheel.wheel.mouse_y = 40.0f;
    REQUIRE(SDL_PushEvent(&wheel));
    REQUIRE(window->PumpEvents().Ok());

    REQUIRE(sink.events.size() == 3);
    CHECK(sink.events[0].type == InputEventType::kMouseMove);
    CHECK(sink.events[0].x == 100);
    CHECK(sink.events[0].y == 50);
    CHECK(sink.events[0].dx == 3);
    CHECK(sink.events[0].dy == -2);
    CHECK(sink.events[1].type == InputEventType::kMouseButtonDown);
    CHECK(sink.events[1].button == MouseButton::kRight);
    CHECK(sink.events[1].x == 10);
    CHECK(sink.events[1].y == 20);
    CHECK(sink.events[2].type == InputEventType::kMouseWheel);
    CHECK(sink.events[2].wheel == 1.0f);
    CHECK(sink.events[2].x == 30);
    CHECK(sink.events[2].y == 40);

    const std::vector<WindowEvent> queued = drain(*window);
    REQUIRE(queued.size() == 3);
    CHECK(queued[0].type == WindowEventType::kMouseMove);
    CHECK(queued[1].type == WindowEventType::kMouseButtonDown);
    CHECK(queued[2].type == WindowEventType::kMouseWheel);
}

TEST_CASE("relative mode holds the position and reports motion only") {
    DummyVideo video;
    auto window = makeWindow();
    REQUIRE(window->Create(smallWindow()).Ok());
    RecordingSink sink;
    REQUIRE(window->AttachInput(sink).Ok());
    const SDL_WindowID id = onlyWindowId();
    pushMotion(id, 200.0f, 100.0f, 0.0f, 0.0f);
    REQUIRE(window->PumpEvents().Ok());

    sink.relative = true;
    sink.events.clear();
    pushMotion(id, 300.0f, 300.0f, 25.0f, -7.0f);
    REQUIRE(window->PumpEvents().Ok());
    REQUIRE(sink.events.size() == 1);
    CHECK(sink.events[0].x == 200);
    CHECK(sink.events[0].y == 100);
    CHECK(sink.events[0].dx == 25);
    CHECK(sink.events[0].dy == -7);
    CHECK(window->DetachInput().Ok());
}

TEST_CASE("focus, minimise, restore and close reach the queue, focus also the sink") {
    DummyVideo video;
    auto window = makeWindow();
    REQUIRE(window->Create(smallWindow()).Ok());
    REQUIRE(window->PumpEvents().Ok());
    drain(*window);
    RecordingSink sink;
    REQUIRE(window->AttachInput(sink).Ok());
    const SDL_WindowID id = onlyWindowId();

    pushWindowEvent(id, SDL_EVENT_WINDOW_FOCUS_LOST);
    pushWindowEvent(id, SDL_EVENT_WINDOW_FOCUS_GAINED);
    pushWindowEvent(id, SDL_EVENT_WINDOW_MINIMIZED);
    pushWindowEvent(id, SDL_EVENT_WINDOW_RESTORED);
    pushWindowEvent(id, SDL_EVENT_WINDOW_CLOSE_REQUESTED);
    REQUIRE(window->PumpEvents().Ok());

    REQUIRE(sink.focus.size() == 2);
    CHECK(sink.focus[0] == false);
    CHECK(sink.focus[1] == true);
    std::vector<WindowEventType> types;
    for (const WindowEvent &event : drain(*window)) {
        types.push_back(event.type);
        CHECK(event.timeMs == 2);
    }
    const std::vector<WindowEventType> expected = {
        WindowEventType::kFocusLost, WindowEventType::kFocusGained, WindowEventType::kMinimized,
        WindowEventType::kRestored, WindowEventType::kCloseRequested};
    CHECK(types == expected);
}

TEST_CASE("typed text is queued one character at a time") {
    DummyVideo video;
    auto window = makeWindow();
    REQUIRE(window->Create(smallWindow()).Ok());
    REQUIRE(window->PumpEvents().Ok());
    drain(*window);
    SDL_Event text;
    SDL_zero(text);
    text.type = SDL_EVENT_TEXT_INPUT;
    text.text.windowID = onlyWindowId();
    text.text.text = "a\xC3\xA9"; // "a", then U+00E9
    REQUIRE(SDL_PushEvent(&text));
    REQUIRE(window->PumpEvents().Ok());
    const std::vector<WindowEvent> queued = drain(*window);
    REQUIRE(queued.size() == 2);
    CHECK(queued[0].type == WindowEventType::kText);
    CHECK(queued[0].codepoint == U'a');
    CHECK(queued[1].codepoint == U'é');
}

TEST_CASE("after DetachInput, events reach only the window queue") {
    DummyVideo video;
    auto window = makeWindow();
    REQUIRE(window->Create(smallWindow()).Ok());
    REQUIRE(window->PumpEvents().Ok());
    drain(*window);
    RecordingSink sink;
    REQUIRE(window->AttachInput(sink).Ok());
    REQUIRE(window->DetachInput().Ok());
    CHECK(window->DetachInput().Ok()); // detaching with none attached succeeds
    pushKey(onlyWindowId(), SDL_EVENT_KEY_DOWN, SDL_SCANCODE_B, 0);
    REQUIRE(window->PumpEvents().Ok());
    CHECK(sink.events.empty());
    const std::vector<WindowEvent> queued = drain(*window);
    REQUIRE(queued.size() == 1);
    CHECK(queued[0].input.key == 0x05);
}

TEST_CASE("Destroy discards the queued events") {
    DummyVideo video;
    auto window = makeWindow();
    REQUIRE(window->Create(smallWindow()).Ok());
    pushKey(onlyWindowId(), SDL_EVENT_KEY_DOWN, SDL_SCANCODE_C, 0);
    REQUIRE(window->PumpEvents().Ok());
    REQUIRE(window->Destroy().Ok());
    auto polled = window->PollEvent();
    REQUIRE(polled.Ok());
    CHECK_FALSE(polled.Value().has_value());
}


TEST_CASE("a Create whose mode cannot be applied leaves no window") {
    DummyVideo video;
    auto window = makeWindow();
    WindowDesc desc = smallWindow();
    desc.mode = WindowMode::kFullscreen;
    failNextWindowModeForTest();
    CHECK(window->Create(desc).Code() == Error::kOutOfResources);
    CHECK(window->GetState().Code() == Error::kWrongState);
    CHECK(window->SetTitle("TestCaptain").Code() == Error::kWrongState);
    CHECK(onlyWindowId() == 0);
    REQUIRE(window->Create(smallWindow()).Ok());
    CHECK(window->GetState().Value().width == 640);
}

TEST_CASE("one close request per close, whether SDL sends one event or two") {
    DummyVideo video;
    auto window = makeWindow();
    REQUIRE(window->Create(smallWindow()).Ok());
    REQUIRE(window->PumpEvents().Ok());
    drain(*window);
    const SDL_WindowID id = onlyWindowId();

    pushWindowEvent(id, SDL_EVENT_WINDOW_CLOSE_REQUESTED);
    SDL_Event quit;
    SDL_zero(quit);
    quit.type = SDL_EVENT_QUIT;
    quit.quit.timestamp = 2000000;
    REQUIRE(SDL_PushEvent(&quit));
    REQUIRE(window->PumpEvents().Ok());
    const std::vector<WindowEvent> first = drain(*window);
    REQUIRE(first.size() == 1);
    CHECK(first[0].type == WindowEventType::kCloseRequested);

    // A QUIT alone (a termination signal) is still a close request, and so is the next click.
    REQUIRE(SDL_PushEvent(&quit));
    REQUIRE(window->PumpEvents().Ok());
    CHECK(drain(*window).size() == 1);
    pushWindowEvent(id, SDL_EVENT_WINDOW_CLOSE_REQUESTED);
    REQUIRE(window->PumpEvents().Ok());
    CHECK(drain(*window).size() == 1);
}

TEST_CASE("the mouse maps through a letterboxed window at scale 2, carrying fractions") {
    DummyVideo video;
    auto window = makeWindow();
    REQUIRE(window->Create(smallWindow()).Ok()); // logical 640x480
    const SDL_WindowID id = onlyWindowId();
    SDL_Window *sdl = SDL_GetWindowFromID(id);
    REQUIRE(sdl != nullptr);
    // 1280x1200 points: scale min(2, 2.5) = 2, content 1280x960, bars of 120 points above and below.
    REQUIRE(SDL_SetWindowSize(sdl, 1280, 1200));
    SDL_SyncWindow(sdl);
    REQUIRE(window->PumpEvents().Ok()); // refreshes the point size
    RecordingSink sink;
    REQUIRE(window->AttachInput(sink).Ok());

    pushMotion(id, 100.0f, 170.0f, 1.0f, 1.0f);   // (100,170) -> logical (50,25); 0.5 carried
    pushMotion(id, 102.0f, 172.0f, 1.0f, 1.0f);   // (102,172) -> (51,26); 0.5+0.5 = 1 whole
    pushMotion(id, 0.0f, 0.0f, 0.0f, 0.0f);       // in the top bar: clamps to the corner
    pushMotion(id, 1279.0f, 1199.0f, 0.0f, 0.0f); // bottom-right bar: clamps to (639,479)
    REQUIRE(window->PumpEvents().Ok());
    REQUIRE(sink.events.size() == 4);
    CHECK(sink.events[0].x == 50);
    CHECK(sink.events[0].y == 25);
    CHECK(sink.events[0].dx == 0);
    CHECK(sink.events[0].dy == 0);
    CHECK(sink.events[1].x == 51);
    CHECK(sink.events[1].y == 26);
    CHECK(sink.events[1].dx == 1);
    CHECK(sink.events[1].dy == 1);
    CHECK(sink.events[2].x == 0);
    CHECK(sink.events[2].y == 0);
    CHECK(sink.events[3].x == 639);
    CHECK(sink.events[3].y == 479);
}

TEST_CASE("with the whole-number scale on, the mouse maps through its bars") {
    DummyVideo video;
    auto window = makeWindow();
    REQUIRE(window->Create(smallWindow()).Ok()); // logical 640x480
    const SDL_WindowID id = onlyWindowId();
    SDL_Window *sdl = SDL_GetWindowFromID(id);
    REQUIRE(sdl != nullptr);
    REQUIRE(SDL_SetWindowSize(sdl, 1600, 1080)); // the dummy driver: a point is a pixel
    SDL_SyncWindow(sdl);
    REQUIRE(window->PumpEvents().Ok());
    REQUIRE(window->GetState().Value().drawableWidth == 1600);
    CHECK(window->GetWholeScale().Value() == false);

    // Off: 1600x1080 holds 640x480 at 2.25x, 1440x1080 at x 80. (160, 60) is logical
    // (80 * 0.444, 60 * 0.444) = (35.6, 26.7); (100, 500) is (8.9, 222.2).
    CHECK(clicksAt(*window, id, {{160.0f, 60.0f}, {100.0f, 500.0f}}) == Points{{35, 26}, {8, 222}});

    // On: 2x, 1280x960 at (160, 60), bars of 160 at the sides and 60 above and below.
    REQUIRE(window->SetWholeScale(true).Ok());
    CHECK(window->GetWholeScale().Value() == true);
    CHECK(clicksAt(*window, id,
                   {{160.0f, 60.0f},     // the frame's top-left corner
                    {1439.0f, 1019.0f},  // its bottom-right pixel: (1279 / 2, 959 / 2)
                    {800.0f, 540.0f},    // the middle: (640 / 2, 480 / 2)
                    {100.0f, 500.0f},    // the left bar: x clamps to 0, y (500 - 60) / 2
                    {1500.0f, 30.0f}}) == // the top-right corner bar: clamped both ways
          Points{{0, 0}, {639, 479}, {320, 240}, {0, 220}, {639, 0}});

    // Relative motion moves at the placement's 2 points per logical pixel.
    RecordingSink sink;
    REQUIRE(window->AttachInput(sink).Ok());
    pushMotion(id, 802.0f, 544.0f, 2.0f, 4.0f);
    REQUIRE(window->PumpEvents().Ok());
    REQUIRE(sink.events.size() == 1);
    CHECK(sink.events[0].x == 321);
    CHECK(sink.events[0].y == 242);
    CHECK(sink.events[0].dx == 1);
    CHECK(sink.events[0].dy == 2);
    REQUIRE(window->DetachInput().Ok());

    REQUIRE(window->SetWholeScale(false).Ok());
    CHECK(clicksAt(*window, id, {{160.0f, 60.0f}}) == Points{{35, 26}});
}

TEST_CASE("a frame the player resized keeps its size when SetMode changes the logical size") {
    DummyVideo video;
    auto window = makeWindow();
    REQUIRE(window->Create(smallWindow()).Ok()); // logical 640x480, frame 640x480
    const SDL_WindowID id = onlyWindowId();
    REQUIRE(window->PumpEvents().Ok());
    drain(*window);

    // The player drags the frame to 1280x720 (a size the backend did not set).
    REQUIRE(SDL_SetWindowSize(SDL_GetWindowFromID(id), 1280, 720));
    SDL_SyncWindow(SDL_GetWindowFromID(id));
    REQUIRE(window->PumpEvents().Ok());
    for (const WindowEvent &event : drain(*window)) {
        CHECK(event.type != WindowEventType::kResized); // a drag is not a logical change
    }

    // The game sets 1024x768: the frame stays, the logical size changes and is reported.
    REQUIRE(window->SetMode(WindowMode::kWindowed, 1024, 768).Ok());
    CHECK(frameSize(id) == std::pair<int, int>{1280, 720});
    CHECK(window->GetState().Value().width == 1024);
    CHECK(window->GetState().Value().height == 768);
    REQUIRE(window->PumpEvents().Ok());
    bool resized = false;
    for (const WindowEvent &event : drain(*window)) {
        if (event.type == WindowEventType::kResized) {
            CHECK(event.width == 1024);
            CHECK(event.height == 768);
            resized = true;
        }
    }
    CHECK(resized);

    // Wider than the frame: 1024x768 shown 960x720 at x 160, bars of 160 points at each side.
    // The middle, a button's edge, and both bars, each clamped to the frame's edge, not dropped.
    CHECK(clicksAt(*window, id, {{640.0f, 360.0f}, {160.0f, 0.0f}, {10.0f, 360.0f},
                                 {1279.0f, 700.0f}}) ==
          Points{{512, 384}, {0, 0}, {0, 384}, {1023, 746}});
}

TEST_CASE("a frame nobody resized takes each logical size, mapping the mouse one to one") {
    DummyVideo video;
    auto window = makeWindow();
    REQUIRE(window->Create(smallWindow()).Ok());
    const SDL_WindowID id = onlyWindowId();
    REQUIRE(window->SetMode(WindowMode::kWindowed, 800, 600).Ok());
    REQUIRE(window->PumpEvents().Ok());
    CHECK(frameSize(id) == std::pair<int, int>{800, 600});
    REQUIRE(window->SetMode(WindowMode::kWindowed, 1024, 768).Ok());
    REQUIRE(window->PumpEvents().Ok());
    CHECK(frameSize(id) == std::pair<int, int>{1024, 768});
    // The frame's own size: game pixels are points.
    CHECK(clicksAt(*window, id, {{512.0f, 384.0f}, {0.0f, 0.0f}, {1023.0f, 767.0f}}) ==
          Points{{512, 384}, {0, 0}, {1023, 767}});
}

TEST_CASE("a frame created with a window size keeps it, and the frame is scaled into it") {
    DummyVideo video;
    auto window = makeWindow();
    WindowDesc desc = smallWindow(); // logical 640x480
    desc.windowWidth = 512;
    desc.windowHeight = 384;
    REQUIRE(window->Create(desc).Ok());
    const SDL_WindowID id = onlyWindowId();
    CHECK(frameSize(id) == std::pair<int, int>{512, 384});
    CHECK(window->GetState().Value().width == 640);
    REQUIRE(window->SetMode(WindowMode::kWindowed, 1024, 768).Ok());
    REQUIRE(window->PumpEvents().Ok());
    CHECK(frameSize(id) == std::pair<int, int>{512, 384});
    CHECK(window->GetState().Value().width == 1024);
    CHECK(window->GetState().Value().height == 768);
    // Smaller than the frame, half its size: each point is two game pixels.
    CHECK(clicksAt(*window, id, {{256.0f, 192.0f}, {0.0f, 0.0f}, {511.0f, 383.0f}}) ==
          Points{{512, 384}, {0, 0}, {1022, 766}});
}

TEST_CASE("a 1x1 frame maps the mouse without dividing by a side rounded to 0") {
    DummyVideo video;
    auto window = makeWindow();
    WindowDesc desc = smallWindow();
    desc.windowWidth = 1;
    desc.windowHeight = 1;
    REQUIRE(window->Create(desc).Ok());
    const SDL_WindowID id = onlyWindowId();
    REQUIRE(frameSize(id) == std::pair<int, int>{1, 1});
    // 640x480 in 1x1: the frame is placed 1x1 (480 + 320 over 640 rounds to 1).
    CHECK(clicksAt(*window, id, {{0.0f, 0.0f}, {0.5f, 0.5f}}) == Points{{0, 0}, {320, 240}});
    // 1024x200 in 1x1: 200 high scales to 0.2 of a point, rounded to 0; mapped as one point.
    REQUIRE(window->SetMode(WindowMode::kWindowed, 1024, 200).Ok());
    REQUIRE(window->PumpEvents().Ok());
    CHECK(frameSize(id) == std::pair<int, int>{1, 1});
    CHECK(clicksAt(*window, id, {{0.0f, 0.0f}, {0.5f, 0.5f}}) == Points{{0, 0}, {512, 100}});
}

TEST_CASE("a window size needs both sides, each within the limits") {
    DummyVideo video;
    auto window = makeWindow();
    WindowDesc desc = smallWindow();
    desc.windowWidth = 800;
    CHECK(window->Create(desc).Code() == Error::kInvalidArgument);
    desc.windowWidth = 0;
    desc.windowHeight = 600;
    CHECK(window->Create(desc).Code() == Error::kInvalidArgument);
    desc.windowWidth = 16385;
    CHECK(window->Create(desc).Code() == Error::kInvalidArgument);
}

} // namespace sfcop::platform
