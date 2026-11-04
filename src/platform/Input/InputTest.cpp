#include "platform/Input/Input.h"
#include "platform/Input/InputMock.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace sfcop::platform {
namespace {

constexpr Scancode kKeyA = 0x04;
constexpr Scancode kKeyEscape = 0x29;

InputEvent keyEvent(InputEventType type, Scancode key, std::uint32_t timeMs,
                    std::uint32_t sequence) {
    InputEvent event;
    event.type = type;
    event.key = key;
    event.timeMs = timeMs;
    event.sequence = sequence;
    return event;
}

// A caller written against the interface only, as a buffered keyboard read is: drains the queue
// into `out` (keys only), and reports whether anything was lost.
bool drainKeys(Input &input, std::vector<InputEvent> &out) {
    for (;;) {
        Result<std::optional<InputEvent>> next = input.PollEvent();
        if (!next.Ok() || !next.Value().has_value()) {
            break;
        }
        const InputEvent &event = *next.Value();
        if (event.type == InputEventType::kKeyDown || event.type == InputEventType::kKeyUp) {
            out.push_back(event);
        }
    }
    return input.TakeOverflow().ValueOr(false);
}

} // namespace

TEST_CASE("a caller drains buffered key events in order through the input interface") {
    InputMock mock;
    mock.events.push_back(keyEvent(InputEventType::kKeyDown, kKeyA, 1000, 1));
    InputEvent motionEvent;
    motionEvent.type = InputEventType::kMouseMove;
    mock.events.push_back(motionEvent);
    mock.events.push_back(keyEvent(InputEventType::kKeyUp, kKeyA, 1016, 3));
    mock.overflow = true;

    std::vector<InputEvent> keys;
    CHECK(drainKeys(mock, keys));
    REQUIRE(keys.size() == 2);
    CHECK(keys[0].key == kKeyA);
    CHECK(keys[0].timeMs == 1000);
    CHECK(keys[1].type == InputEventType::kKeyUp);
    CHECK(keys[1].sequence == 3);
    CHECK(mock.log.Calls() == std::vector<std::string>{"PollEvent()", "PollEvent()", "PollEvent()",
                                                       "PollEvent()", "TakeOverflow()"});
    CHECK_FALSE(mock.overflow);
}

TEST_CASE("the input mock answers immediate keyboard and mouse reads from its programmed state") {
    InputMock mock;
    Input &input = mock;
    mock.keyboard[kKeyEscape] = true;
    mock.motion = MouseMotion{3, -2, 1.0f};
    CHECK(input.IsKeyDown(kKeyEscape).Value());
    CHECK_FALSE(input.IsKeyDown(kKeyA).Value());
    CHECK(input.IsKeyDown(300).Code() == Error::kInvalidArgument);
    MouseMotion first = input.TakeMouseMotion().Value();
    CHECK(first.dx == 3);
    CHECK(first.dy == -2);
    CHECK(input.TakeMouseMotion().Value().dx == 0);
    CHECK(input.SetRelativeMode(true).Ok());
    CHECK(input.IsRelativeMode().Value());
    mock.failWith = Error::kWrongState;
    CHECK(input.GetMouse().Code() == Error::kWrongState);
}

TEST_CASE("a window delivering to an input as a sink fills its queue, numbered by the input") {
    InputMock mock;
    InputSink &sink = mock;
    InputEvent delivered = keyEvent(InputEventType::kKeyDown, kKeyA, 200, 999);
    sink.OnInputEvent(delivered);
    sink.OnInputEvent(keyEvent(InputEventType::kKeyUp, kKeyA, 216, 999));
    CHECK_FALSE(sink.WantsRelativeMouse());
    mock.relativeMode = true;
    CHECK(sink.WantsRelativeMouse());
    std::vector<InputEvent> keys;
    CHECK_FALSE(drainKeys(mock, keys));
    REQUIRE(keys.size() == 2);
    CHECK(keys[0].sequence == 1);
    CHECK(keys[1].sequence == 2);
    CHECK(keys[1].timeMs == 216);
}

TEST_CASE("an input that loses focus reads every key and button up") {
    InputMock mock;
    mock.keyboard[kKeyEscape] = true;
    mock.mouse.buttons[static_cast<std::size_t>(MouseButton::kLeft)] = true;
    mock.OnFocusChanged(false);
    CHECK_FALSE(mock.IsKeyDown(kKeyEscape).Value());
    CHECK_FALSE(mock.GetMouse().Value().buttons[0]);
}

TEST_CASE("every method of the input stub reports unsupported") {
    std::unique_ptr<Input> input = makeInputStub();
    input->OnInputEvent(keyEvent(InputEventType::kKeyDown, kKeyA, 0, 0));
    input->OnFocusChanged(true);
    CHECK_FALSE(input->WantsRelativeMouse());
    CHECK(input->GetKeyboard().Code() == Error::kUnsupported);
    CHECK(input->IsKeyDown(kKeyA).Code() == Error::kUnsupported);
    CHECK(input->GetMouse().Code() == Error::kUnsupported);
    CHECK(input->TakeMouseMotion().Code() == Error::kUnsupported);
    CHECK(input->PollEvent().Code() == Error::kUnsupported);
    CHECK(input->TakeOverflow().Code() == Error::kUnsupported);
    CHECK(input->SetQueueCapacity(64).Code() == Error::kUnsupported);
    CHECK(input->FlushEvents().Code() == Error::kUnsupported);
    CHECK(input->SetRelativeMode(true).Code() == Error::kUnsupported);
    CHECK(input->IsRelativeMode().Code() == Error::kUnsupported);
}

} // namespace sfcop::platform
