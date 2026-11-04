// The SDL3 Input backend, driven as a window drives it: hand-written event sequences pushed
// through the InputSink side and read back through every Input method.
#include "platform/Input/Input.h"

#include <SDL3/SDL_scancode.h>
#include <doctest/doctest.h>

#include <vector>

namespace sfcop::platform {
namespace {

constexpr Scancode kKeyA = 0x04;
constexpr Scancode kKeyS = 0x16;
constexpr Scancode kKeyReturn = 0x28;
constexpr Scancode kKeyLeftCtrl = 0xE0;

InputEvent key(InputEventType type, Scancode code, std::uint32_t timeMs) {
    InputEvent event;
    event.type = type;
    event.key = code;
    event.timeMs = timeMs;
    event.sequence = 777; // the sink numbers events itself; this must be ignored
    return event;
}

InputEvent mouseMove(std::int32_t x, std::int32_t y, std::int32_t dx, std::int32_t dy) {
    InputEvent event;
    event.type = InputEventType::kMouseMove;
    event.x = x;
    event.y = y;
    event.dx = dx;
    event.dy = dy;
    return event;
}

InputEvent button(InputEventType type, MouseButton which, std::int32_t x, std::int32_t y) {
    InputEvent event;
    event.type = type;
    event.button = which;
    event.x = x;
    event.y = y;
    return event;
}

std::vector<InputEvent> drain(Input &input) {
    std::vector<InputEvent> out;
    for (;;) {
        std::optional<InputEvent> next = input.PollEvent().Value();
        if (!next) {
            return out;
        }
        out.push_back(*next);
    }
}

} // namespace

TEST_CASE("SDL3 scancodes are the USB usage IDs Input names keys by, so the window passes them "
          "through") {
    CHECK(SDL_SCANCODE_A == 0x04);
    CHECK(SDL_SCANCODE_Z == 0x1D);
    CHECK(SDL_SCANCODE_1 == 0x1E);
    CHECK(SDL_SCANCODE_0 == 0x27);
    CHECK(SDL_SCANCODE_RETURN == 0x28);
    CHECK(SDL_SCANCODE_ESCAPE == 0x29);
    CHECK(SDL_SCANCODE_SPACE == 0x2C);
    CHECK(SDL_SCANCODE_F1 == 0x3A);
    CHECK(SDL_SCANCODE_F12 == 0x45);
    CHECK(SDL_SCANCODE_PRINTSCREEN == 0x46);
    CHECK(SDL_SCANCODE_RIGHT == 0x4F);
    CHECK(SDL_SCANCODE_UP == 0x52);
    CHECK(SDL_SCANCODE_KP_DIVIDE == 0x54);
    CHECK(SDL_SCANCODE_KP_ENTER == 0x58);
    CHECK(SDL_SCANCODE_KP_PERIOD == 0x63);
    CHECK(SDL_SCANCODE_NONUSBACKSLASH == 0x64);
    CHECK(SDL_SCANCODE_APPLICATION == 0x65);
    CHECK(SDL_SCANCODE_LCTRL == 0xE0);
    CHECK(SDL_SCANCODE_RGUI == 0xE7);
}

TEST_CASE("key events set the keyboard state and queue in order, numbered by the input from 1") {
    std::unique_ptr<Input> input = makeInput();
    input->OnInputEvent(key(InputEventType::kKeyDown, kKeyA, 1000));
    input->OnInputEvent(key(InputEventType::kKeyDown, kKeyLeftCtrl, 1005));

    KeyboardState state = input->GetKeyboard().Value();
    CHECK(state[kKeyA]);
    CHECK(state[kKeyLeftCtrl]);
    CHECK_FALSE(state[kKeyS]);
    CHECK(input->IsKeyDown(kKeyA).Value());
    CHECK_FALSE(input->IsKeyDown(kKeyReturn).Value());
    CHECK(input->IsKeyDown(256).Code() == Error::kInvalidArgument);

    input->OnInputEvent(key(InputEventType::kKeyUp, kKeyA, 1050));
    CHECK_FALSE(input->IsKeyDown(kKeyA).Value());

    std::vector<InputEvent> events = drain(*input);
    REQUIRE(events.size() == 3);
    CHECK(events[0].type == InputEventType::kKeyDown);
    CHECK(events[0].key == kKeyA);
    CHECK(events[0].timeMs == 1000);
    CHECK(events[0].sequence == 1);
    CHECK(events[1].key == kKeyLeftCtrl);
    CHECK(events[1].sequence == 2);
    CHECK(events[2].type == InputEventType::kKeyUp);
    CHECK(events[2].timeMs == 1050);
    CHECK(events[2].sequence == 3);
    CHECK_FALSE(input->PollEvent().Value().has_value());
    CHECK_FALSE(input->TakeOverflow().Value());
}

TEST_CASE("an auto-repeat is queued with its flag and leaves the key down") {
    std::unique_ptr<Input> input = makeInput();
    input->OnInputEvent(key(InputEventType::kKeyDown, kKeyS, 10));
    InputEvent repeat = key(InputEventType::kKeyDown, kKeyS, 510);
    repeat.repeat = true;
    input->OnInputEvent(repeat);
    CHECK(input->IsKeyDown(kKeyS).Value());
    std::vector<InputEvent> events = drain(*input);
    REQUIRE(events.size() == 2);
    CHECK_FALSE(events[0].repeat);
    CHECK(events[1].repeat);
}

TEST_CASE("a full queue keeps the earliest events, drops the newest and reports it once") {
    std::unique_ptr<Input> input = makeInput();
    REQUIRE(input->SetQueueCapacity(2).Ok());
    input->OnInputEvent(key(InputEventType::kKeyDown, kKeyA, 1));
    input->OnInputEvent(key(InputEventType::kKeyUp, kKeyA, 2));
    input->OnInputEvent(key(InputEventType::kKeyDown, kKeyS, 3));
    // The dropped event still changes the immediate state.
    CHECK(input->IsKeyDown(kKeyS).Value());

    std::vector<InputEvent> events = drain(*input);
    REQUIRE(events.size() == 2);
    CHECK(events[0].timeMs == 1);
    CHECK(events[1].timeMs == 2);
    CHECK(input->TakeOverflow().Value());
    CHECK_FALSE(input->TakeOverflow().Value());

    // Room again: the next event is queued, numbered after the last one queued.
    input->OnInputEvent(key(InputEventType::kKeyUp, kKeyS, 4));
    events = drain(*input);
    REQUIRE(events.size() == 1);
    CHECK(events[0].sequence == 3);
}

TEST_CASE("the queue holds 64 events by default") {
    std::unique_ptr<Input> input = makeInput();
    for (std::uint32_t i = 0; i < 65; ++i) {
        input->OnInputEvent(
            key(i % 2 == 0 ? InputEventType::kKeyDown : InputEventType::kKeyUp, kKeyA, i));
    }
    CHECK(drain(*input).size() == 64);
    CHECK(input->TakeOverflow().Value());
}

TEST_CASE("setting the queue capacity empties it and checks its argument") {
    std::unique_ptr<Input> input = makeInput();
    input->OnInputEvent(key(InputEventType::kKeyDown, kKeyA, 1));
    CHECK(input->SetQueueCapacity(0).Code() == Error::kInvalidArgument);
    CHECK(input->PollEvent().Value().has_value()); // a refused call changes nothing
    CHECK(input->SetQueueCapacity(65537).Code() == Error::kOutOfResources);
    CHECK(input->SetQueueCapacity(65536).Ok());
    CHECK_FALSE(input->PollEvent().Value().has_value());
    CHECK(input->IsKeyDown(kKeyA).Value()); // the state is not the queue
}

TEST_CASE("flushing discards the queue and the overflow flag but not the state") {
    std::unique_ptr<Input> input = makeInput();
    REQUIRE(input->SetQueueCapacity(1).Ok());
    input->OnInputEvent(key(InputEventType::kKeyDown, kKeyA, 1));
    input->OnInputEvent(key(InputEventType::kKeyDown, kKeyS, 2));
    CHECK(input->FlushEvents().Ok());
    CHECK_FALSE(input->PollEvent().Value().has_value());
    CHECK_FALSE(input->TakeOverflow().Value());
    CHECK(input->IsKeyDown(kKeyA).Value());
    CHECK(input->IsKeyDown(kKeyS).Value());
}

TEST_CASE("losing focus releases every key and button without queueing up events") {
    std::unique_ptr<Input> input = makeInput();
    input->OnInputEvent(key(InputEventType::kKeyDown, kKeyA, 1));
    input->OnInputEvent(key(InputEventType::kKeyDown, kKeyReturn, 2));
    input->OnInputEvent(button(InputEventType::kMouseButtonDown, MouseButton::kRight, 40, 50));
    input->OnFocusChanged(false);

    KeyboardState state = input->GetKeyboard().Value();
    for (std::size_t i = 0; i < kScancodeCount; ++i) {
        CHECK_FALSE(state[i]);
    }
    CHECK_FALSE(input->IsKeyDown(kKeyReturn).Value());
    MouseState mouse = input->GetMouse().Value();
    CHECK_FALSE(mouse.buttons[static_cast<std::size_t>(MouseButton::kRight)]);
    CHECK(mouse.x == 40);
    CHECK(drain(*input).size() == 3); // the three downs, and no ups

    // A key event arriving while unfocused still reads up.
    input->OnInputEvent(key(InputEventType::kKeyDown, kKeyS, 3));
    CHECK_FALSE(input->IsKeyDown(kKeyS).Value());
    input->OnInputEvent(button(InputEventType::kMouseButtonDown, MouseButton::kLeft, 41, 51));

    input->OnFocusChanged(true);
    CHECK_FALSE(input->IsKeyDown(kKeyS).Value()); // no real press happened
    CHECK_FALSE(input->GetMouse().Value().buttons[static_cast<std::size_t>(MouseButton::kLeft)]);
    CHECK_FALSE(input->IsKeyDown(kKeyA).Value()); // nothing comes back with the focus
    input->OnInputEvent(key(InputEventType::kKeyDown, kKeyA, 4));
    CHECK(input->IsKeyDown(kKeyA).Value());
}

TEST_CASE("mouse events set position and buttons and accumulate motion and wheel until taken") {
    std::unique_ptr<Input> input = makeInput();
    input->OnInputEvent(mouseMove(100, 80, 5, -3));
    input->OnInputEvent(mouseMove(110, 70, 10, -10));
    input->OnInputEvent(button(InputEventType::kMouseButtonDown, MouseButton::kLeft, 111, 71));
    InputEvent wheel;
    wheel.type = InputEventType::kMouseWheel;
    wheel.wheel = 1.5f;
    input->OnInputEvent(wheel);
    wheel.wheel = -0.5f;
    input->OnInputEvent(wheel);

    MouseState mouse = input->GetMouse().Value();
    CHECK(mouse.x == 111);
    CHECK(mouse.y == 71);
    CHECK(mouse.buttons[static_cast<std::size_t>(MouseButton::kLeft)]);
    CHECK_FALSE(mouse.buttons[static_cast<std::size_t>(MouseButton::kMiddle)]);

    MouseMotion motion = input->TakeMouseMotion().Value();
    CHECK(motion.dx == 15);
    CHECK(motion.dy == -13);
    CHECK(motion.wheel == doctest::Approx(1.0f));
    MouseMotion again = input->TakeMouseMotion().Value();
    CHECK(again.dx == 0);
    CHECK(again.dy == 0);
    CHECK(again.wheel == 0.0f);

    input->OnInputEvent(button(InputEventType::kMouseButtonUp, MouseButton::kLeft, 112, 72));
    CHECK_FALSE(input->GetMouse().Value().buttons[0]);

    std::vector<InputEvent> events = drain(*input);
    REQUIRE(events.size() == 6);
    CHECK(events[2].type == InputEventType::kMouseButtonDown);
    CHECK(events[5].sequence == 6);
}

TEST_CASE("relative mode is what the window reads, and holds the position while motion counts") {
    std::unique_ptr<Input> input = makeInput();
    CHECK_FALSE(input->WantsRelativeMouse());
    CHECK_FALSE(input->IsRelativeMode().Value());
    input->OnInputEvent(mouseMove(200, 150, 0, 0));

    CHECK(input->SetRelativeMode(true).Ok());
    CHECK(input->WantsRelativeMouse());
    CHECK(input->IsRelativeMode().Value());
    input->OnInputEvent(mouseMove(200, 150, 30, 40));
    input->OnInputEvent(mouseMove(900, 900, -5, 2));
    MouseState mouse = input->GetMouse().Value();
    CHECK(mouse.x == 200);
    CHECK(mouse.y == 150);
    MouseMotion motion = input->TakeMouseMotion().Value();
    CHECK(motion.dx == 25);
    CHECK(motion.dy == 42);

    CHECK(input->SetRelativeMode(false).Ok());
    CHECK_FALSE(input->WantsRelativeMouse());
    input->OnInputEvent(mouseMove(201, 151, 1, 1));
    CHECK(input->GetMouse().Value().x == 201);
}

} // namespace sfcop::platform
