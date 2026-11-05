// ScriptedWindow: a Window that adds an input script's events to the inner window's own, so a
// scripted click reaches the game exactly as a real one does (DESIGN §5.10).
//
// It wraps the real Window (or the stub, headless) and forwards every call. PumpEvents() pumps
// the inner window first, then counts one frame, advances the ScriptPlayer with that frame and
// Clock::Ticks(), and treats each due event as the operating system's: queued for PollEvent()
// and delivered to the attached InputSink (OnInputEvent, OnFocusChanged), in order. PollEvent()
// hands out the inner window's events before the script's, so a real event that arrived in the
// same frame keeps its place. The one drain of the Window (the guest host's pump) therefore feeds
// both USER32's message queue and Input (DirectInput) with the script's events, and neither the
// host nor the modules know a script is playing.
//
// PollEvent() is called only from the host's pump, on the thread PumpEvents() runs on (the
// guest host's GuestPump is the one drain of the Window); it does not check the thread itself.
//
// A frame is one turn of the game's message pump (one PumpEvents() call). Gpu::Present() is not
// counted: headless, DirectDraw is not the module's and nothing presents.
//
// Before a scripted click or key, a window without the operating system's focus gets a
// kFocusGained first, as the Wine driver brings the game's window to the front before it clicks.
//
// Headless, the inner stub fails every call: PumpEvents() then still plays the script and
// succeeds; AttachInput() keeps the sink and reports the inner window's answer.
//
// Threading: PumpEvents() runs only on the thread that called BindToCallingThread(); from any
// other it does nothing and fails with kWrongState. The script fails the run through `onFailure`,
// called once, on that thread, when a wait runs out.
#pragma once

#include "app/input/ScriptPlayer.h"
#include "platform/Clock/Clock.h"
#include "platform/Window/Window.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <thread>

namespace sfcop::app::input {

class ScriptedWindow final : public platform::Window {
public:
    ScriptedWindow(platform::Window &inner, ScriptPlayer &player, platform::Clock &clock,
                   std::function<void(const PlayerFailure &)> onFailure);

    // Makes the calling thread the one PumpEvents() works on (the game's main thread).
    void BindToCallingThread();

    // Frames counted so far.
    std::uint64_t Frames() const { return frame_; }

    // The window it wraps (ScriptedGpu attaches the Gpu to it).
    platform::Window &Inner() const { return inner_; }

    platform::Status Create(const platform::WindowDesc &desc) override;
    platform::Status Destroy() override;
    platform::Status SetMode(platform::WindowMode mode, std::uint32_t width,
                             std::uint32_t height) override;
    platform::Result<platform::WindowState> GetState() override;
    platform::Result<platform::DisplayMode> GetDesktopMode() override;
    platform::Result<std::vector<platform::DisplayMode>> ListDisplayModes() override;
    platform::Status SetTitle(std::string_view title) override;
    platform::Status SetCursorVisible(bool visible) override;
    platform::Status SetFrameCap(platform::FrameCap cap) override;
    platform::Result<platform::FrameCap> GetFrameCap() override;
    platform::Status AttachInput(platform::InputSink &sink) override;
    platform::Status DetachInput() override;
    platform::Status PumpEvents() override;
    platform::Result<std::optional<platform::WindowEvent>> PollEvent() override;

private:
    void deliver(const platform::WindowEvent &event);

    platform::Window &inner_;
    ScriptPlayer &player_;
    platform::Clock &clock_;
    std::function<void(const PlayerFailure &)> onFailure_;
    platform::InputSink *sink_ = nullptr;
    std::deque<platform::WindowEvent> scripted_;
    std::thread::id thread_{};
    bool bound_ = false;
    bool failureReported_ = false;
    std::uint64_t frame_ = 0;
};

} // namespace sfcop::app::input
