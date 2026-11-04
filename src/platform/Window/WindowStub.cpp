// The stub backend: compiles on every platform and supports nothing.
#include "platform/Window/Window.h"

namespace sfcop::platform {
namespace {

class WindowStub final : public Window {
public:
    Status Create(const WindowDesc &) override { return Error::kUnsupported; }
    Status Destroy() override { return Error::kUnsupported; }
    Status SetMode(WindowMode, std::uint32_t, std::uint32_t) override {
        return Error::kUnsupported;
    }
    Result<WindowState> GetState() override { return Error::kUnsupported; }
    Result<DisplayMode> GetDesktopMode() override { return Error::kUnsupported; }
    Result<std::vector<DisplayMode>> ListDisplayModes() override { return Error::kUnsupported; }
    Status SetTitle(std::string_view) override { return Error::kUnsupported; }
    Status SetCursorVisible(bool) override { return Error::kUnsupported; }
    Status SetFrameCap(FrameCap) override { return Error::kUnsupported; }
    Result<FrameCap> GetFrameCap() override { return Error::kUnsupported; }
    Status AttachInput(InputSink &) override { return Error::kUnsupported; }
    Status DetachInput() override { return Error::kUnsupported; }
    Status PumpEvents() override { return Error::kUnsupported; }
    Result<std::optional<WindowEvent>> PollEvent() override { return Error::kUnsupported; }
};

} // namespace

std::unique_ptr<Window> makeWindowStub() { return std::make_unique<WindowStub>(); }

} // namespace sfcop::platform
