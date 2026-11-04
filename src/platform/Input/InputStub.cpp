// The stub backend: compiles on every platform and supports nothing.
#include "platform/Input/Input.h"

namespace sfcop::platform {
namespace {

class InputStub final : public Input {
public:
    void OnInputEvent(const InputEvent &) override {}
    void OnFocusChanged(bool) override {}
    bool WantsRelativeMouse() const override { return false; }
    Result<KeyboardState> GetKeyboard() override { return Error::kUnsupported; }
    Result<bool> IsKeyDown(Scancode) override { return Error::kUnsupported; }
    Result<MouseState> GetMouse() override { return Error::kUnsupported; }
    Result<MouseMotion> TakeMouseMotion() override { return Error::kUnsupported; }
    Result<std::optional<InputEvent>> PollEvent() override { return Error::kUnsupported; }
    Result<bool> TakeOverflow() override { return Error::kUnsupported; }
    Status SetQueueCapacity(std::size_t) override { return Error::kUnsupported; }
    Status FlushEvents() override { return Error::kUnsupported; }
    Status SetRelativeMode(bool) override { return Error::kUnsupported; }
    Result<bool> IsRelativeMode() override { return Error::kUnsupported; }
};

} // namespace

std::unique_ptr<Input> makeInputStub() { return std::make_unique<InputStub>(); }

} // namespace sfcop::platform
