// WindowManager: the game's windows as USER32 keeps them (DESIGN §4.7, §5.6). Classes, windows
// and their rectangles, styles, long values, text, focus and activation, the invalid regions
// WM_PAINT reports, menus as bookkeeping, the clipboard on a host buffer, hooks as records, the
// cursor, and the screen the game sees.
//
// The screen is the game's display (ScreenInfo, the display mode DirectDraw set, 640x480 before
// it does), as on the machine the game was written for; GetSystemMetrics and GetWindowRect speak
// its coordinates. The platform Window is the game's active top-level window: its title follows
// SetWindowTextA, its cursor ShowCursor; its size follows DirectDraw's display mode
// (hle/ddraw4), never SetWindowPos, which changes only the game's view of its rectangle.
//
// Nothing here calls the game. The messages a call sends (WM_CREATE, WM_ACTIVATE and the rest)
// are sent by User32, which owns this manager; this class only says what changed.
//
// Not thread-safe: the guest layer calls it under the machine lock.
#pragma once

#include "hle/gdi32/Gdi.h"
#include "platform/Window/Window.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace sfcop::hle::user32 {

using gdi32::Rect;

// The window procedures this module runs itself. A window whose procedure is one of these is
// never handed to the game; GetWindowLongA(GWL_WNDPROC) returns the value so that the game can
// chain to it with CallWindowProcA. They lie in a range no guest code occupies.
constexpr std::uint32_t kDefWindowProc = 0xFFFF0010u;
constexpr std::uint32_t kDialogProc = 0xFFFF0020u;
constexpr std::uint32_t kButtonProc = 0xFFFF0030u;
constexpr std::uint32_t kEditProc = 0xFFFF0040u;
constexpr std::uint32_t kStaticProc = 0xFFFF0050u;
constexpr bool isModuleProc(std::uint32_t proc) {
    return proc >= 0xFFFF0000u;
}

struct WindowClass {
    std::string name; // lower case: class names match without case
    std::uint32_t style = 0;
    std::uint32_t proc = 0;
    std::int32_t classExtra = 0;
    std::int32_t windowExtra = 0;
    std::uint32_t instance = 0;
    std::uint32_t icon = 0;
    std::uint32_t cursor = 0;
    std::uint32_t background = 0;
    std::uint16_t atom = 0;
};

struct WindowRecord {
    std::uint32_t handle = 0;
    std::string className;
    std::uint32_t proc = 0;
    std::uint32_t style = 0;
    std::uint32_t exStyle = 0;
    std::uint32_t parent = 0; // a child's parent; a top-level window's owner
    std::uint32_t id = 0;     // a child's id; a top-level window's menu
    std::uint32_t instance = 0;
    std::uint32_t userData = 0;
    std::string text;
    Rect rect; // screen coordinates for a top-level window, the parent's client for a child
    bool visible = false;
    bool enabled = true;
    bool dialog = false;
    bool destroying = false;
    std::vector<std::uint32_t> extra; // cbWndExtra, in 4-byte words (DWL_* for a dialog)
    bool invalid = false;
    Rect invalidRect;
};

struct CreateParams {
    std::uint32_t exStyle = 0;
    std::string className; // or "#<atom>"
    std::string windowName;
    std::uint32_t style = 0;
    std::int32_t x = 0, y = 0, width = 0, height = 0;
    std::uint32_t parent = 0;
    std::uint32_t menuOrId = 0;
    std::uint32_t instance = 0;
};

struct MenuItem {
    std::uint32_t flags = 0;
    std::uint32_t id = 0;
    std::string text;
};

// The width and height of the frame a style gives a window, on each side, and the caption.
struct Frame {
    std::int32_t side = 0;
    std::int32_t top = 0; // includes the caption
};
Frame frameOf(std::uint32_t style, std::uint32_t exStyle);

enum class UserHandle : std::uint8_t { kWindow, kMenu, kHook };

// What the manager needs from its host. In the guest, handles are entries of win32hle's handle
// table of this module's types; in tests, counters (countingUserHandles()).
struct WindowHost {
    std::function<gdi32::ScreenInfo()> screen;
    std::function<std::uint32_t(UserHandle)> allocate; // 0: none left
    std::function<void(std::uint32_t)> release;
    std::function<bool()> onMainThread; // the platform Window's main-thread calls are made only then
};

// Windows from 0x3000, menus from 0x4000, hooks from 0x5000, four apart; always the main thread.
WindowHost countingUserHandles(std::function<gdi32::ScreenInfo()> screen);

class WindowManager {
public:
    WindowManager(platform::Window &window, WindowHost host);

    // ---- classes ----
    // The class's atom, or 0 if the name is taken or empty.
    std::uint16_t RegisterClass(const WindowClass &windowClass);
    const WindowClass *FindClass(const std::string &nameOrAtom) const;

    // ---- windows ----
    // The new window's handle, or 0 for an unknown class or no handle left. The record is made
    // hidden; User32 sends the creation messages and shows it if WS_VISIBLE asks.
    std::uint32_t Create(const CreateParams &params);
    // Removes a window and its children, children first; returns the handles removed in that
    // order (User32 sends WM_DESTROY before calling this).
    std::vector<std::uint32_t> Destroy(std::uint32_t handle);
    WindowRecord *Find(std::uint32_t handle);
    const WindowRecord *Find(std::uint32_t handle) const;
    bool IsWindow(std::uint32_t handle) const { return Find(handle) != nullptr; }
    std::vector<std::uint32_t> Children(std::uint32_t parent) const;
    std::uint32_t ChildWithId(std::uint32_t parent, std::uint32_t id) const;
    // FindWindowA: the first top-level window of the class and title given (either may be empty).
    std::uint32_t FindTopLevel(const std::string &className, const std::string &title) const;
    std::vector<std::uint32_t> TopLevelWindows() const;

    // ---- geometry ----
    Rect WindowRect(std::uint32_t handle) const;  // screen coordinates
    Rect ClientRect(std::uint32_t handle) const;  // 0, 0, width, height
    // The client area's top-left corner on the screen.
    void ClientOrigin(std::uint32_t handle, std::int32_t &x, std::int32_t &y) const;
    // AdjustWindowRect: the window rectangle for a client rectangle and style.
    static Rect AdjustRect(const Rect &client, std::uint32_t style, bool menu,
                           std::uint32_t exStyle = 0);
    // SetWindowPos's position and size part (flags SWP_NOMOVE, SWP_NOSIZE).
    bool SetPosition(std::uint32_t handle, std::int32_t x, std::int32_t y, std::int32_t width,
                     std::int32_t height, std::uint32_t flags);

    // ---- long values ----
    std::optional<std::uint32_t> GetLong(std::uint32_t handle, std::int32_t index) const;
    // The previous value, or nothing for a bad handle or index.
    std::optional<std::uint32_t> SetLong(std::uint32_t handle, std::int32_t index,
                                         std::uint32_t value);

    // ---- text, visibility, focus ----
    bool SetText(std::uint32_t handle, const std::string &text);
    // Whether the window was visible before.
    bool SetVisible(std::uint32_t handle, bool visible);
    std::uint32_t Active() const { return active_; }
    std::uint32_t Focus() const { return focus_; }
    // The previous active window (0 if none).
    std::uint32_t SetActive(std::uint32_t handle);
    std::uint32_t SetFocus(std::uint32_t handle);
    // The top-level window a window belongs to.
    std::uint32_t TopLevelOf(std::uint32_t handle) const;
    // IsIconic: the platform window's minimised state for the active window, else false.
    bool IsIconic(std::uint32_t handle);

    // ---- painting ----
    void Invalidate(std::uint32_t handle, const Rect *rect);
    void Validate(std::uint32_t handle);
    // The first visible window with an invalid region (of `filter` and its children if nonzero).
    std::uint32_t NextInvalid(std::uint32_t filter) const;

    // ---- menus (bookkeeping) ----
    std::uint32_t CreateMenu();
    bool AppendMenu(std::uint32_t menu, std::uint32_t flags, std::uint32_t id, std::string text);
    // CheckMenuItem: the previous check state, or 0xFFFFFFFF for no such item.
    std::uint32_t CheckMenuItem(std::uint32_t menu, std::uint32_t item, std::uint32_t flags);
    bool DeleteMenu(std::uint32_t menu, std::uint32_t item, std::uint32_t flags);
    bool IsMenu(std::uint32_t menu) const { return menus_.count(menu) != 0; }
    std::uint32_t SystemMenu(std::uint32_t handle);
    const std::vector<MenuItem> *MenuItems(std::uint32_t menu) const;

    // ---- clipboard ----
    bool OpenClipboard(std::uint32_t owner);
    bool CloseClipboard();
    bool EmptyClipboard();
    bool SetClipboard(std::uint32_t format, std::vector<std::uint8_t> data);
    const std::vector<std::uint8_t> *Clipboard(std::uint32_t format) const;

    // ---- hooks ----
    std::uint32_t AddHook(std::int32_t type, std::uint32_t proc);
    bool RemoveHook(std::uint32_t hook);
    std::size_t HookCount() const { return hooks_.size(); }

    // ---- cursor ----
    // ShowCursor: the new display count; the platform cursor is shown while it is not negative.
    std::int32_t ShowCursor(bool show);
    std::uint32_t SetCursor(std::uint32_t cursor);

    gdi32::ScreenInfo Screen() const { return host_.screen(); }
    platform::Window &HostWindow() const { return window_; }
    bool OnMainThread() const { return host_.onMainThread(); }

private:
    // The frame, with a menu bar on top for a top-level window that has a menu.
    Frame frameWithMenu(const WindowRecord &record) const;

    struct Hook {
        std::int32_t type;
        std::uint32_t proc;
    };

    platform::Window &window_;
    WindowHost host_;
    std::map<std::uint16_t, WindowClass> classes_;
    std::uint16_t nextAtom_ = 0xC000;
    std::map<std::uint32_t, WindowRecord> windows_;
    std::vector<std::uint32_t> order_; // creation order
    std::uint32_t active_ = 0, focus_ = 0;
    std::map<std::uint32_t, std::vector<MenuItem>> menus_;
    std::map<std::uint32_t, std::uint32_t> systemMenus_;
    bool clipboardOpen_ = false;
    std::map<std::uint32_t, std::vector<std::uint8_t>> clipboard_;
    std::map<std::uint32_t, Hook> hooks_;
    std::int32_t cursorCount_ = 0;
    std::uint32_t cursor_ = 0;
};

} // namespace sfcop::hle::user32
