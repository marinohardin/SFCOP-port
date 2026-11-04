// The Window backend on SDL3 (DESIGN §5.6, §5.7, §6.2): one SDL window, SDL's event queue read by
// PumpEvents() alone, and the frame cap Gpu::Present() reads.
//
// Logical size: the game's pixels. In windowed mode the window is that many points; in
// fullscreen the content is scaled to the display keeping its aspect, so mouse positions are
// mapped back through the same letterbox. A user dragging the frame changes the drawable size,
// not the logical size, and queues no kResized; only SetMode() changes the logical size.
//
// Fullscreen covers the display at its desktop mode. With SFCOP_FULLSCREEN=exclusive in the
// environment, a display mode of exactly the requested size is switched to when the display
// offers one. With SFCOP_WINDOW_HIDDEN=1 the window is created hidden, for tests and headless
// runs.
//
// Event timestamps are SDL's (milliseconds since SDL was initialised), the origin a Clock on
// SDL_GetTicks() shares.
#include "platform/Window/Window.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>

namespace sfcop::platform {
namespace {

constexpr std::uint32_t kMaxWindowSize = 16384;
constexpr std::uint32_t kMaxFrameCap = 1000;

// The property on the SDL window that names the Window object owning it. Gpu::Attach() finds its
// SDL window through it, which is how a Gpu on SDL accepts only a Window of this backend.
constexpr const char *kOwnerProperty = "sfcop.platform.window";

// Test hook (declared by the test, not in Window.h): the next ApplyMode() fails, so that the
// teardown of a Create() whose mode cannot be applied can be exercised on a driver that accepts
// everything.
std::atomic<bool> gFailNextApplyMode{false};

bool envIs(const char *name, const char *value) {
    const char *set = std::getenv(name);
    return set != nullptr && std::strcmp(set, value) == 0;
}

bool validSize(std::uint32_t width, std::uint32_t height) {
    return width != 0 && height != 0 && width <= kMaxWindowSize && height <= kMaxWindowSize;
}

std::uint32_t ticksOf(Uint64 timestampNs) {
    return static_cast<std::uint32_t>(timestampNs / 1000000u);
}

DisplayMode toDisplayMode(const SDL_DisplayMode &mode) {
    DisplayMode out;
    out.width = static_cast<std::uint32_t>(mode.w);
    out.height = static_cast<std::uint32_t>(mode.h);
    out.refreshHz =
        mode.refresh_rate > 0.0f ? static_cast<std::uint32_t>(std::lround(mode.refresh_rate)) : 0;
    return out;
}

bool toMouseButton(Uint8 button, MouseButton &out) {
    switch (button) {
    case SDL_BUTTON_LEFT:
        out = MouseButton::kLeft;
        return true;
    case SDL_BUTTON_RIGHT:
        out = MouseButton::kRight;
        return true;
    case SDL_BUTTON_MIDDLE:
        out = MouseButton::kMiddle;
        return true;
    case SDL_BUTTON_X1:
        out = MouseButton::kX1;
        return true;
    case SDL_BUTTON_X2:
        out = MouseButton::kX2;
        return true;
    default:
        return false;
    }
}

class WindowSdl final : public Window {
public:
    WindowSdl() { videoReady_ = SDL_InitSubSystem(SDL_INIT_VIDEO); }

    ~WindowSdl() override {
        (void)WindowSdl::Destroy();
        if (videoReady_) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    }

    Status Create(const WindowDesc &desc) override {
        if (window_ != nullptr) {
            return Error::kWrongState;
        }
        if (!validSize(desc.width, desc.height)) {
            return Error::kInvalidArgument;
        }
        if (!videoReady_) {
            return Error::kOutOfResources;
        }
        SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
        if (desc.mode == WindowMode::kWindowed && desc.resizable) {
            flags |= SDL_WINDOW_RESIZABLE;
        }
        if (envIs("SFCOP_WINDOW_HIDDEN", "1")) {
            flags |= SDL_WINDOW_HIDDEN;
        }
        window_ = SDL_CreateWindow(desc.title.c_str(), static_cast<int>(desc.width),
                                   static_cast<int>(desc.height), flags);
        if (window_ == nullptr) {
            return Error::kOutOfResources;
        }
        windowId_ = SDL_GetWindowID(window_);
        SDL_SetPointerProperty(SDL_GetWindowProperties(window_), kOwnerProperty,
                               static_cast<Window *>(this));
        SDL_StartTextInput(window_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = WindowState{};
            state_.mode = WindowMode::kWindowed;
            state_.width = desc.width;
            state_.height = desc.height;
        }
        if (desc.mode == WindowMode::kFullscreen &&
            !ApplyMode(WindowMode::kFullscreen, desc.width, desc.height)) {
            (void)WindowSdl::Destroy(); // the same teardown as a normal close
            return Error::kOutOfResources;
        }
        RefreshState();
        return {};
    }

    Status Destroy() override {
        if (window_ == nullptr) {
            return {};
        }
        if (relativeApplied_) {
            SDL_SetWindowRelativeMouseMode(window_, false);
            relativeApplied_ = false;
        }
        sink_ = nullptr;
        SDL_SetPointerProperty(SDL_GetWindowProperties(window_), kOwnerProperty, nullptr);
        SDL_StopTextInput(window_);
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        windowId_ = 0;
        resizePending_ = false;
        closeQueuedThisPump_ = false;
        mouseX_ = 0;
        mouseY_ = 0;
        residualX_ = 0.0f;
        residualY_ = 0.0f;
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        state_ = WindowState{};
        pointWidth_ = 0;
        pointHeight_ = 0;
        return {};
    }

    Status SetMode(WindowMode mode, std::uint32_t width, std::uint32_t height) override {
        if (window_ == nullptr) {
            return Error::kWrongState;
        }
        if (!validSize(width, height)) {
            return Error::kInvalidArgument;
        }
        if (!ApplyMode(mode, width, height)) {
            return Error::kUnsupported;
        }
        RefreshState();
        resizePending_ = true;
        return {};
    }

    Result<WindowState> GetState() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_.width == 0) {
            return Error::kWrongState;
        }
        return state_;
    }

    Result<DisplayMode> GetDesktopMode() override {
        const SDL_DisplayID display = CurrentDisplay();
        if (display == 0) {
            return Error::kIoError;
        }
        const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(display);
        if (mode == nullptr) {
            return Error::kIoError;
        }
        return toDisplayMode(*mode);
    }

    Result<std::vector<DisplayMode>> ListDisplayModes() override {
        const SDL_DisplayID display = CurrentDisplay();
        if (display == 0) {
            return Error::kIoError;
        }
        int count = 0;
        SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(display, &count);
        if (modes == nullptr) {
            return Error::kIoError;
        }
        std::vector<DisplayMode> out;
        out.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            out.push_back(toDisplayMode(*modes[i]));
        }
        SDL_free(modes);
        sortModes(out);
        return out;
    }

    Status SetTitle(std::string_view title) override {
        if (window_ == nullptr) {
            return Error::kWrongState;
        }
        const std::string text(title);
        SDL_SetWindowTitle(window_, text.c_str());
        return {};
    }

    Status SetCursorVisible(bool visible) override {
        if (window_ == nullptr) {
            return Error::kWrongState;
        }
        if (visible) {
            SDL_ShowCursor();
        } else {
            SDL_HideCursor();
        }
        return {};
    }

    Status SetFrameCap(FrameCap cap) override {
        if (cap.maxFramesPerSecond > kMaxFrameCap) {
            return Error::kInvalidArgument;
        }
        frameCap_.store(cap.maxFramesPerSecond);
        return {};
    }

    Result<FrameCap> GetFrameCap() override { return FrameCap{frameCap_.load()}; }

    Status AttachInput(InputSink &sink) override {
        sink_ = &sink;
        return {};
    }

    Status DetachInput() override {
        sink_ = nullptr;
        if (relativeApplied_ && window_ != nullptr) {
            SDL_SetWindowRelativeMouseMode(window_, false);
        }
        relativeApplied_ = false;
        return {};
    }

    Status PumpEvents() override {
        if (window_ != nullptr) {
            const bool wantRelative = sink_ != nullptr && sink_->WantsRelativeMouse();
            if (wantRelative != relativeApplied_) {
                if (SDL_SetWindowRelativeMouseMode(window_, wantRelative)) {
                    relativeApplied_ = wantRelative;
                } else if (!relativeFailureLogged_) {
                    // Logging goes through SDL's log until the shared facility exists.
                    relativeFailureLogged_ = true;
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "relative mouse mode refused: %s", SDL_GetError());
                }
            }
        }
        closeQueuedThisPump_ = false;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            Handle(event);
        }
        if (window_ != nullptr) {
            RefreshState();
            if (resizePending_) {
                resizePending_ = false;
                WindowEvent resized;
                resized.type = WindowEventType::kResized;
                resized.timeMs = ticksOf(SDL_GetTicksNS());
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    resized.width = state_.width;
                    resized.height = state_.height;
                }
                Queue(resized);
            }
        }
        return {};
    }

    Result<std::optional<WindowEvent>> PollEvent() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return std::optional<WindowEvent>{};
        }
        WindowEvent event = queue_.front();
        queue_.pop_front();
        return std::optional<WindowEvent>{event};
    }

private:
    static void sortModes(std::vector<DisplayMode> &modes) {
        const auto larger = [](const DisplayMode &a, const DisplayMode &b) {
            const std::uint64_t areaA = std::uint64_t{a.width} * a.height;
            const std::uint64_t areaB = std::uint64_t{b.width} * b.height;
            if (areaA != areaB) {
                return areaA > areaB;
            }
            if (a.width != b.width) {
                return a.width > b.width;
            }
            return a.refreshHz > b.refreshHz;
        };
        std::sort(modes.begin(), modes.end(), larger);
        const auto same = [](const DisplayMode &a, const DisplayMode &b) {
            return a.width == b.width && a.height == b.height && a.refreshHz == b.refreshHz;
        };
        modes.erase(std::unique(modes.begin(), modes.end(), same), modes.end());
    }

    SDL_DisplayID CurrentDisplay() const {
        if (!videoReady_) {
            return 0;
        }
        if (window_ != nullptr) {
            const SDL_DisplayID display = SDL_GetDisplayForWindow(window_);
            if (display != 0) {
                return display;
            }
        }
        return SDL_GetPrimaryDisplay();
    }

    bool ApplyMode(WindowMode mode, std::uint32_t width, std::uint32_t height) {
        if (gFailNextApplyMode.exchange(false)) {
            return false;
        }
        if (mode == WindowMode::kWindowed) {
            if (!SDL_SetWindowFullscreen(window_, false)) {
                return false;
            }
            SDL_SetWindowSize(window_, static_cast<int>(width), static_cast<int>(height));
        } else {
            SDL_DisplayMode exact;
            const SDL_DisplayMode *chosen = nullptr;
            if (envIs("SFCOP_FULLSCREEN", "exclusive") &&
                SDL_GetClosestFullscreenDisplayMode(CurrentDisplay(), static_cast<int>(width),
                                                    static_cast<int>(height), 0.0f, true,
                                                    &exact) &&
                exact.w == static_cast<int>(width) && exact.h == static_cast<int>(height)) {
                chosen = &exact;
            }
            if (!SDL_SetWindowFullscreenMode(window_, chosen) ||
                !SDL_SetWindowFullscreen(window_, true)) {
                return false;
            }
        }
        SDL_SyncWindow(window_);
        std::lock_guard<std::mutex> lock(mutex_);
        state_.mode = mode;
        state_.width = width;
        state_.height = height;
        return true;
    }

    // Reads the drawable size, focus and minimised state into the copy GetState() returns, so
    // that GetState() never touches SDL from another thread.
    void RefreshState() {
        int pixelW = 0;
        int pixelH = 0;
        SDL_GetWindowSizeInPixels(window_, &pixelW, &pixelH);
        int pointW = 0;
        int pointH = 0;
        SDL_GetWindowSize(window_, &pointW, &pointH);
        const SDL_WindowFlags flags = SDL_GetWindowFlags(window_);
        std::lock_guard<std::mutex> lock(mutex_);
        state_.drawableWidth = static_cast<std::uint32_t>(std::max(pixelW, 0));
        state_.drawableHeight = static_cast<std::uint32_t>(std::max(pixelH, 0));
        state_.focused = (flags & SDL_WINDOW_INPUT_FOCUS) != 0;
        state_.minimized = (flags & SDL_WINDOW_MINIMIZED) != 0;
        pointWidth_ = pointW;
        pointHeight_ = pointH;
    }

    // Window points to logical pixels through the letterbox Gpu::Present() draws.
    void ToLogical(float x, float y, std::int32_t &outX, std::int32_t &outY) const {
        float logicalW = 0.0f;
        float logicalH = 0.0f;
        float pointW = 0.0f;
        float pointH = 0.0f;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            logicalW = static_cast<float>(state_.width);
            logicalH = static_cast<float>(state_.height);
            pointW = static_cast<float>(pointWidth_);
            pointH = static_cast<float>(pointHeight_);
        }
        if (logicalW <= 0.0f || logicalH <= 0.0f || pointW <= 0.0f || pointH <= 0.0f) {
            outX = static_cast<std::int32_t>(std::floor(x));
            outY = static_cast<std::int32_t>(std::floor(y));
            return;
        }
        const float scale = (std::min)(pointW / logicalW, pointH / logicalH);
        const float offsetX = (pointW - logicalW * scale) * 0.5f;
        const float offsetY = (pointH - logicalH * scale) * 0.5f;
        const float lx = std::clamp((x - offsetX) / scale, 0.0f, logicalW - 1.0f);
        const float ly = std::clamp((y - offsetY) / scale, 0.0f, logicalH - 1.0f);
        outX = static_cast<std::int32_t>(std::floor(lx));
        outY = static_cast<std::int32_t>(std::floor(ly));
    }

    float LogicalScale() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_.width == 0 || state_.height == 0 || pointWidth_ <= 0 || pointHeight_ <= 0) {
            return 1.0f;
        }
        return (std::min)(static_cast<float>(pointWidth_) / static_cast<float>(state_.width),
                          static_cast<float>(pointHeight_) / static_cast<float>(state_.height));
    }

    void Queue(const WindowEvent &event) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(event);
    }

    void QueueSimple(WindowEventType type, Uint64 timestampNs) {
        WindowEvent event;
        event.type = type;
        event.timeMs = ticksOf(timestampNs);
        Queue(event);
    }

    // Queues the window event and delivers its input event to the sink.
    void Deliver(WindowEventType type, const InputEvent &input) {
        WindowEvent event;
        event.type = type;
        event.timeMs = input.timeMs;
        event.input = input;
        Queue(event);
        if (sink_ != nullptr) {
            sink_->OnInputEvent(input);
        }
    }

    bool Ours(SDL_WindowID id) const { return id == 0 || id == windowId_; }

    void Handle(const SDL_Event &event) {
        if (window_ == nullptr) {
            return;
        }
        switch (event.type) {
        case SDL_EVENT_QUIT:
            // SDL sends QUIT after the last window's close request, and also for a termination
            // signal. One close, one kCloseRequested: a QUIT in the same pump as a close request
            // is that request's echo.
            if (!closeQueuedThisPump_) {
                closeQueuedThisPump_ = true;
                QueueSimple(WindowEventType::kCloseRequested, event.quit.timestamp);
            }
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (Ours(event.window.windowID) && !closeQueuedThisPump_) {
                closeQueuedThisPump_ = true;
                QueueSimple(WindowEventType::kCloseRequested, event.window.timestamp);
            }
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            if (Ours(event.window.windowID)) {
                const bool gained = event.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
                QueueSimple(gained ? WindowEventType::kFocusGained : WindowEventType::kFocusLost,
                            event.window.timestamp);
                if (sink_ != nullptr) {
                    sink_->OnFocusChanged(gained);
                }
            }
            break;
        case SDL_EVENT_WINDOW_MINIMIZED:
            if (Ours(event.window.windowID)) {
                QueueSimple(WindowEventType::kMinimized, event.window.timestamp);
            }
            break;
        case SDL_EVENT_WINDOW_RESTORED:
            if (Ours(event.window.windowID)) {
                QueueSimple(WindowEventType::kRestored, event.window.timestamp);
            }
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            if (Ours(event.key.windowID) && event.key.scancode < kScancodeCount) {
                InputEvent input;
                const bool down = event.type == SDL_EVENT_KEY_DOWN;
                input.type = down ? InputEventType::kKeyDown : InputEventType::kKeyUp;
                input.timeMs = ticksOf(event.key.timestamp);
                input.key = static_cast<Scancode>(event.key.scancode);
                input.repeat = event.key.repeat;
                input.x = mouseX_;
                input.y = mouseY_;
                Deliver(down ? WindowEventType::kKeyDown : WindowEventType::kKeyUp, input);
            }
            break;
        case SDL_EVENT_TEXT_INPUT:
            if (Ours(event.text.windowID) && event.text.text != nullptr) {
                const char *text = event.text.text;
                std::size_t left = std::strlen(text);
                while (left > 0) {
                    const Uint32 codepoint = SDL_StepUTF8(&text, &left);
                    if (codepoint == 0) {
                        break;
                    }
                    WindowEvent typed;
                    typed.type = WindowEventType::kText;
                    typed.timeMs = ticksOf(event.text.timestamp);
                    typed.codepoint = static_cast<char32_t>(codepoint);
                    Queue(typed);
                }
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (Ours(event.motion.windowID)) {
                HandleMotion(event.motion);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (Ours(event.button.windowID)) {
                MouseButton button;
                if (!toMouseButton(event.button.button, button)) {
                    break;
                }
                const bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                if (!relativeApplied_) {
                    ToLogical(event.button.x, event.button.y, mouseX_, mouseY_);
                }
                InputEvent input;
                input.type = down ? InputEventType::kMouseButtonDown : InputEventType::kMouseButtonUp;
                input.timeMs = ticksOf(event.button.timestamp);
                input.button = button;
                input.x = mouseX_;
                input.y = mouseY_;
                Deliver(down ? WindowEventType::kMouseButtonDown : WindowEventType::kMouseButtonUp,
                        input);
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (Ours(event.wheel.windowID)) {
                if (!relativeApplied_) {
                    ToLogical(event.wheel.mouse_x, event.wheel.mouse_y, mouseX_, mouseY_);
                }
                InputEvent input;
                input.type = InputEventType::kMouseWheel;
                input.timeMs = ticksOf(event.wheel.timestamp);
                input.x = mouseX_;
                input.y = mouseY_;
                input.wheel = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y
                                                                               : event.wheel.y;
                Deliver(WindowEventType::kMouseWheel, input);
            }
            break;
        default:
            break;
        }
    }

    // Motion in logical pixels. Fractions are carried to the next event, so that slow motion on a
    // scaled window is not lost.
    void HandleMotion(const SDL_MouseMotionEvent &motion) {
        const float scale = LogicalScale();
        residualX_ += motion.xrel / scale;
        residualY_ += motion.yrel / scale;
        const float wholeX = std::trunc(residualX_);
        const float wholeY = std::trunc(residualY_);
        residualX_ -= wholeX;
        residualY_ -= wholeY;
        if (!relativeApplied_) {
            ToLogical(motion.x, motion.y, mouseX_, mouseY_);
        }
        InputEvent input;
        input.type = InputEventType::kMouseMove;
        input.timeMs = ticksOf(motion.timestamp);
        input.x = mouseX_;
        input.y = mouseY_;
        input.dx = static_cast<std::int32_t>(wholeX);
        input.dy = static_cast<std::int32_t>(wholeY);
        Deliver(WindowEventType::kMouseMove, input);
    }

    bool videoReady_ = false;
    SDL_Window *window_ = nullptr;
    SDL_WindowID windowId_ = 0;
    InputSink *sink_ = nullptr;
    bool relativeApplied_ = false;
    bool resizePending_ = false;
    bool closeQueuedThisPump_ = false;
    bool relativeFailureLogged_ = false;
    std::atomic<std::uint32_t> frameCap_{0};

    // Main thread only: the mouse position as last delivered, and the motion fractions.
    std::int32_t mouseX_ = 0;
    std::int32_t mouseY_ = 0;
    float residualX_ = 0.0f;
    float residualY_ = 0.0f;

    // Read from any thread: guarded by mutex_.
    mutable std::mutex mutex_;
    WindowState state_;
    int pointWidth_ = 0;
    int pointHeight_ = 0;
    std::deque<WindowEvent> queue_;
};

} // namespace

// Declared in WindowSdlTest.cpp only.
void failNextWindowModeForTest() { gFailNextApplyMode.store(true); }

std::unique_ptr<Window> makeWindow() { return std::make_unique<WindowSdl>(); }

} // namespace sfcop::platform
