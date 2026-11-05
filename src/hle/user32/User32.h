// User32: the USER32 rows the game imports, on the Window and Input components and this
// project's GDI (DESIGN §4.7, §5.6). It owns the WindowManager and the MessageQueue and runs the
// messages: the window procedures are the game's, called through GuestCalls; the procedures of
// the module's own classes (DefWindowProc, the dialog class, the controls) run here.
//
// The pump. One place drains the platform Window: the host's pump (src/app/guest/GuestPump.h). On
// the game's main thread, each PeekMessageA, GetMessageA and modal-dialog loop runs the host's
// pump, which reads the Window's events and hands each to OnWindowEvent (-> MessageQueue::Feed),
// then does its own DirectInput and screenshot work. This module never calls Window::PumpEvents
// itself. A thread other than the main thread does not pump, and GetMessageA on such a thread
// takes nothing from the queue (see Get).
//
// A modal dialog is drawn with GDI onto a bitmap the Presenter puts on the screen. When nothing
// can show it (a headless run, or a Presenter that shows nothing: no DirectDraw primary yet),
// the dialog answers with the first enabled push button's id in template order, logging only
// "dialog answered: button <id>"; MessageBoxA answers the same way, except that a Retry button
// is pressed at most three times and then the Cancel (or Abort) button is.
//
// Not thread-safe: the guest layer calls it under the machine lock.
#pragma once

#include "hle/gdi32/Gdi.h"
#include "hle/user32/Dialog.h"
#include "hle/user32/MessageQueue.h"
#include "hle/user32/WindowManager.h"
#include "platform/Gpu/Gpu.h"
#include "platform/Input/Input.h"
#include "platform/Window/Window.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sfcop::hle::user32 {

// The game's code and memory, as User32 reaches them (the guest shims in the game; a fake in
// tests).
class GuestCalls {
public:
    virtual ~GuestCalls() = default;
    // Calls a window, dialog or hook procedure of the game (stdcall, four arguments).
    virtual std::uint32_t CallProc(std::uint32_t proc, std::uint32_t hwnd, std::uint32_t message,
                                   std::uint32_t wParam, std::uint32_t lParam) = 0;
    // Writes `text` and its terminator into the game's buffer of `size` bytes (truncating);
    // returns the characters written, without the terminator.
    virtual std::uint32_t WriteText(std::uint32_t address, std::uint32_t size,
                                    const std::string &text) = 0;
    // Gives the machine up for about `ms` milliseconds, so the game's other threads run.
    virtual void Idle(std::uint32_t ms) = 0;
};

// Puts a dialog's image on the screen at (x, y), screen coordinates; pixels are 0x00RRGGBB, top
// row first. Whether anything was shown (false: no display to draw on).
class Presenter {
public:
    virtual ~Presenter() = default;
    virtual bool Show(const std::vector<std::uint32_t> &pixels, std::int32_t width,
                      std::int32_t height, std::int32_t x, std::int32_t y) = 0;
};

// The texture the game's display is in, for the Presenter on Gpu.
struct DisplayTarget {
    platform::TextureId texture;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    platform::PixelFormat format = platform::PixelFormat::kXrgb8888;
};

// A Presenter that writes the dialog into the display texture and presents it. `display` names
// the texture; when it names none, nothing is shown.
std::unique_ptr<Presenter> makeGpuPresenter(platform::Gpu &gpu,
                                            std::function<std::optional<DisplayTarget>()> display);

// Converts 0x00RRGGBB pixels to a texture format (kXrgb8888, kArgb8888, kRgb565, kArgb1555);
// nothing for another format.
std::optional<std::vector<std::uint8_t>> pixelsAs(const std::vector<std::uint32_t> &pixels,
                                                  platform::PixelFormat format);

// What the guest runtime gives the module (AttachHost). The components are held by reference for
// the life of the host's Run(): they outlive every row the game can call.
struct Host {
    platform::Window *window = nullptr;
    platform::Input *input = nullptr;
    platform::Gpu *gpu = nullptr;
    // The host's own pump: what it does on each turn of the game's message loop. It reads the
    // Window's events and gives each to DeliverEvent.
    std::function<void()> pump;
    // The game's display, for dialogs; absent or answering nothing in a headless run.
    std::function<std::optional<DisplayTarget>()> display;
    // Whether the host is shutting down (GetMessageA on a thread other than the main one returns
    // WM_QUIT then). Absent: never.
    std::function<bool()> stopping;
};

// Gives the module its components; called once, on the game's main thread, before any guest code
// runs. A second call is an error: it is logged, nothing changes, and false comes back (the host
// fails the run).
bool AttachHost(Host host);
// The host given; nothing before AttachHost (a row that finds none logs it and fails the run).
const Host *AttachedHost();
// Whether the calling thread is the one AttachHost was called on.
bool OnMainThread();

// The host's pump gives each platform event here, on the main thread. They reach the message queue
// once the User32 exists (made by the first row the game calls); until then they are kept, in
// order, to a limit of 256 (older ones are dropped).
void DeliverEvent(const platform::WindowEvent &event);
// The User32's side: the sink for the events (it first receives those kept).
void SetEventSink(std::function<void(const platform::WindowEvent &)> sink);

// What GetMessageA and PeekMessageA return to the game.
struct Retrieved {
    bool found = false;
    Message message;
};

class User32 {
public:
    User32(platform::Window &window, platform::Input &input, gdi32::Gdi &gdi, GuestCalls &guest,
           WindowHost handles, std::function<std::uint32_t()> now);

    WindowManager &Windows() { return windows_; }
    MessageQueue &Queue() { return queue_; }

    // The host's pump, and the dialog presenter.
    void SetHostPump(std::function<void()> pump) { hostPump_ = std::move(pump); }
    void SetPresenter(std::unique_ptr<Presenter> presenter) { presenter_ = std::move(presenter); }
    void SetStopping(std::function<bool()> stopping) { stopping_ = std::move(stopping); }

    // Runs the host's pump (main thread only), which gives the platform's events to
    // OnWindowEvent.
    void Pump();
    // One platform event into the queue (what the host's pump calls, through DeliverEvent). Main
    // thread only.
    void OnWindowEvent(const platform::WindowEvent &event);

    // ---- classes and windows ----
    std::uint16_t RegisterClass(const WindowClass &windowClass);
    // CreateWindowExA: `createStruct` is the CREATESTRUCTA (in the game's memory) the creation
    // messages carry.
    std::uint32_t CreateWindow(const CreateParams &params, std::uint32_t createStruct);
    bool DestroyWindow(std::uint32_t hwnd);
    bool ShowWindow(std::uint32_t hwnd, std::uint32_t command);
    bool SetWindowPos(std::uint32_t hwnd, std::int32_t x, std::int32_t y, std::int32_t width,
                      std::int32_t height, std::uint32_t flags);
    std::uint32_t SetActiveWindow(std::uint32_t hwnd);
    std::uint32_t SetFocus(std::uint32_t hwnd);
    bool SetWindowText(std::uint32_t hwnd, const std::string &text);
    std::uint32_t SetWindowLong(std::uint32_t hwnd, std::int32_t index, std::uint32_t value);

    // ---- messages ----
    std::uint32_t Send(std::uint32_t hwnd, std::uint32_t message, std::uint32_t wParam,
                       std::uint32_t lParam);
    bool Post(std::uint32_t hwnd, std::uint32_t message, std::uint32_t wParam,
              std::uint32_t lParam);
    // PeekMessageA (pumps first).
    Retrieved Peek(std::uint32_t hwnd, std::uint32_t first, std::uint32_t last, bool remove);
    // GetMessageA: on the main thread, waits, pumping, until a message comes; message.message is
    // WM_QUIT for quit. On another thread (the queue is one, the main thread's) it takes nothing
    // and never pumps: it waits until the host is shutting down, then returns WM_QUIT.
    Message Get(std::uint32_t hwnd, std::uint32_t first, std::uint32_t last);
    std::uint32_t Dispatch(const Message &message);
    bool Translate(const Message &message) { return queue_.Translate(message); }
    // CallWindowProcA: a procedure of the game or of this module.
    std::uint32_t CallProc(std::uint32_t proc, std::uint32_t hwnd, std::uint32_t message,
                           std::uint32_t wParam, std::uint32_t lParam);
    std::uint32_t DefWindowProc(std::uint32_t hwnd, std::uint32_t message, std::uint32_t wParam,
                                std::uint32_t lParam);

    // ---- dialogs ----
    // DialogBoxIndirectParamA: runs the dialog until EndDialog; -1 if it cannot be made.
    std::int32_t DialogBox(const DialogTemplate &dialog, std::uint32_t owner,
                           std::uint32_t instance, std::uint32_t dialogProc, std::uint32_t param);
    bool EndDialog(std::uint32_t hwnd, std::int32_t result);
    // MessageBoxA's answer.
    std::int32_t MessageBox(std::uint32_t owner, const std::string &text,
                            const std::string &caption, std::uint32_t type);

    // ---- painting ----
    // BeginPaint: the DC (the screen's) and the region; validates the window.
    std::uint32_t BeginPaint(std::uint32_t hwnd, Rect &paint, bool &erase);
    bool EndPaint(std::uint32_t hwnd, std::uint32_t dc);
    // GetDC: a DC on the screen (the game draws through DirectDraw; GDI on a window is unseen).
    std::uint32_t GetDC(std::uint32_t hwnd);
    bool ReleaseDC(std::uint32_t hwnd, std::uint32_t dc);

    // ---- input ----
    void CursorPos(std::int32_t &x, std::int32_t &y) const { queue_.Cursor(x, y); }
    std::uint16_t AsyncKeyState(std::uint32_t vk);
    std::uint16_t KeyState(std::uint32_t vk) const { return queue_.KeyState(vk); }

    // ---- the screen ----
    std::int32_t SystemMetric(std::uint32_t index) const;
    // EnumDisplaySettingsA's modes: index, or kEnumCurrentSettings for the current one.
    std::optional<platform::DisplayMode> DisplayMode(std::uint32_t index, std::uint32_t &bits) const;

private:
    std::uint32_t moduleProc(std::uint32_t proc, std::uint32_t hwnd, std::uint32_t message,
                             std::uint32_t wParam, std::uint32_t lParam);
    std::uint32_t dialogProc(std::uint32_t hwnd, std::uint32_t message, std::uint32_t wParam,
                             std::uint32_t lParam);
    void activate(std::uint32_t hwnd);
    Dialog *dialogOf(std::uint32_t hwnd);
    std::int32_t runDialog(Dialog &dialog, std::uint32_t owner);
    bool presentDialog(const Dialog &dialog);
    std::int32_t answerUnseen(Dialog &dialog);
    void routeToDialog(Dialog &dialog, const Message &message);

    platform::Window &window_;
    platform::Input &input_;
    gdi32::Gdi &gdi_;
    GuestCalls &guest_;
    WindowManager windows_;
    MessageQueue queue_;
    std::function<void()> hostPump_;
    std::function<bool()> stopping_;
    std::uint32_t retriesAnswered_ = 0; // MessageBoxA's Retry answers with nobody to see it
    std::unique_ptr<Presenter> presenter_;
    std::vector<std::unique_ptr<Dialog>> dialogs_; // innermost last
    HostSize hostSize_;
};

} // namespace sfcop::hle::user32
