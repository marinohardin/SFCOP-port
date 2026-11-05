#include "hle/user32/User32TestRig.h"
#include "hle/user32/WindowManager.h"

#include <doctest/doctest.h>

namespace sfcop::hle::user32 {

using test::kGameProc;
using test::kOtherProc;
using test::Rig;

namespace {

struct Plain {
    platform::WindowMock window;
    WindowManager windows{window, countingUserHandles([] { return gdi32::ScreenInfo{}; })};

    std::uint32_t Make(std::uint32_t style, std::uint32_t parent = 0, std::uint32_t id = 0,
                       Rect rect = Rect{10, 20, 110, 220}) {
        WindowClass windowClass;
        windowClass.name = "Plain";
        windowClass.proc = kGameProc;
        windowClass.windowExtra = 8;
        windows.RegisterClass(windowClass);
        CreateParams params;
        params.className = "PLAIN"; // class names match without case
        params.style = style;
        params.x = rect.left, params.y = rect.top;
        params.width = rect.right - rect.left, params.height = rect.bottom - rect.top;
        params.parent = parent;
        params.menuOrId = id;
        return windows.Create(params);
    }
};

} // namespace

TEST_CASE("RegisterClassA refuses a name already registered, without case") {
    Plain p;
    WindowClass one;
    one.name = "Orion";
    CHECK(p.windows.RegisterClass(one) == 0xC000);
    one.name = "ORION";
    CHECK(p.windows.RegisterClass(one) == 0);
    one.name = "";
    CHECK(p.windows.RegisterClass(one) == 0);
    REQUIRE(p.windows.FindClass("orion") != nullptr);
    CHECK(p.windows.FindClass("#49152") == p.windows.FindClass("Orion"));
    CHECK(p.windows.FindClass("Nothing") == nullptr);
    REQUIRE(p.windows.FindClass("#32770") != nullptr); // the dialog class, by its atom
    CHECK(p.windows.FindClass("#32770")->proc == kDialogProc);
}

TEST_CASE("CreateWindowExA makes a hidden record; an unknown class or parent fails") {
    Plain p;
    const std::uint32_t hwnd = p.Make(kWsPopup | kWsVisible);
    CHECK(hwnd == 0x3004);
    const WindowRecord *record = p.windows.Find(hwnd);
    REQUIRE(record != nullptr);
    CHECK_FALSE(record->visible);
    CHECK(record->proc == kGameProc);
    CHECK(record->className == "plain");
    CHECK(record->extra.size() == 2);

    CreateParams unknown;
    unknown.className = "Nothing";
    CHECK(p.windows.Create(unknown) == 0);
    CHECK(p.Make(kWsChild, 0x9999) == 0);
}

TEST_CASE("CW_USEDEFAULT places a window at the origin, three quarters of the screen") {
    Plain p;
    WindowClass c;
    c.name = "D";
    p.windows.RegisterClass(c);
    CreateParams params;
    params.className = "D";
    params.x = params.y = params.width = params.height = kCwUseDefault;
    const std::uint32_t hwnd = p.windows.Create(params);
    const Rect r = p.windows.WindowRect(hwnd);
    CHECK(r.left == 0);
    CHECK(r.top == 0);
    CHECK(r.right == 480);
    CHECK(r.bottom == 360);
}

TEST_CASE("A captioned, bordered window's client area is inside its frame") {
    Plain p;
    const std::uint32_t hwnd = p.Make(kWsCaption | kWsThickFrame, 0, 0, Rect{100, 50, 300, 250});
    const Rect client = p.windows.ClientRect(hwnd);
    CHECK(client.left == 0);
    CHECK(client.right == 200 - 8);
    CHECK(client.bottom == 200 - 4 - 18 - 4);
    std::int32_t x = 0, y = 0;
    p.windows.ClientOrigin(hwnd, x, y);
    CHECK(x == 104);
    CHECK(y == 50 + 4 + 18);

    const Rect adjusted =
        WindowManager::AdjustRect(Rect{0, 0, 640, 480}, kWsCaption | kWsThickFrame, false);
    CHECK(adjusted.left == -4);
    CHECK(adjusted.top == -22);
    CHECK(adjusted.right == 644);
    CHECK(adjusted.bottom == 484);
    const Rect popup = WindowManager::AdjustRect(Rect{0, 0, 640, 480}, kWsPopup, true);
    CHECK(popup.top == -19); // a menu bar only
    CHECK(popup.right == 640);
}

TEST_CASE("A top-level window with a menu has a menu bar above its client area") {
    Plain p;
    const std::uint32_t hwnd = p.Make(kWsPopup, 0, 0, Rect{0, 0, 640, 499});
    p.windows.Find(hwnd)->id = p.windows.CreateMenu();
    const Rect client = p.windows.ClientRect(hwnd);
    CHECK(client.right == 640);
    CHECK(client.bottom == 480);
    std::int32_t x = 0, y = 0;
    p.windows.ClientOrigin(hwnd, x, y);
    CHECK(y == 19);
}

TEST_CASE("A child's rectangle is in its parent's client coordinates") {
    Plain p;
    const std::uint32_t parent = p.Make(kWsPopup | kWsBorder, 0, 0, Rect{100, 100, 300, 300});
    const std::uint32_t child = p.Make(kWsChild, parent, 7, Rect{10, 10, 50, 30});
    const Rect r = p.windows.WindowRect(child);
    CHECK(r.left == 111);
    CHECK(r.top == 111);
    CHECK(r.right == 151);
    CHECK(p.windows.ChildWithId(parent, 7) == child);
    CHECK(p.windows.ChildWithId(parent, 8) == 0);
    CHECK(p.windows.TopLevelOf(child) == parent);
    CHECK(p.windows.TopLevelWindows() == std::vector<std::uint32_t>{parent});
}

TEST_CASE("Destroying a window removes its children first and its owned windows") {
    Plain p;
    const std::uint32_t parent = p.Make(kWsPopup);
    const std::uint32_t child = p.Make(kWsChild, parent);
    const std::uint32_t owned = p.Make(kWsPopup, parent);
    p.windows.SetActive(parent);
    p.windows.SetFocus(child);
    const std::vector<std::uint32_t> removed = p.windows.Destroy(parent);
    CHECK(removed == std::vector<std::uint32_t>{child, owned, parent});
    CHECK_FALSE(p.windows.IsWindow(parent));
    CHECK(p.windows.Active() == 0);
    CHECK(p.windows.Focus() == 0);
    CHECK(p.windows.Destroy(parent).empty());
}

TEST_CASE("GetWindowLongA and SetWindowLongA: the indices, the extra bytes, a bad index") {
    Plain p;
    const std::uint32_t hwnd = p.Make(kWsPopup | kWsVisible);
    CHECK(p.windows.GetLong(hwnd, kGwlWndProc) == kGameProc);
    CHECK(p.windows.SetLong(hwnd, kGwlWndProc, kOtherProc) == kGameProc);
    CHECK(p.windows.Find(hwnd)->proc == kOtherProc);
    CHECK(p.windows.GetLong(hwnd, kGwlStyle) == kWsPopup); // hidden until shown
    p.windows.SetVisible(hwnd, true);
    CHECK(p.windows.GetLong(hwnd, kGwlStyle) == (kWsPopup | kWsVisible));
    CHECK(p.windows.SetLong(hwnd, 4, 0x1234) == 0u);
    CHECK(p.windows.GetLong(hwnd, 4) == 0x1234u);
    CHECK_FALSE(p.windows.GetLong(hwnd, 8));
    CHECK_FALSE(p.windows.GetLong(hwnd, 2));
    CHECK_FALSE(p.windows.SetLong(0x9999, kGwlUserData, 1));
    CHECK(p.windows.SetLong(hwnd, kGwlUserData, 5) == 0u);
    CHECK(p.windows.GetLong(hwnd, kGwlUserData) == 5u);
}

TEST_CASE("The active window's text is the platform window's title") {
    Plain p;
    const std::uint32_t hwnd = p.Make(kWsPopup);
    p.windows.SetText(hwnd, "Before");
    CHECK(p.window.log.Calls().empty());
    p.windows.SetActive(hwnd);
    p.windows.SetText(hwnd, "Starfleet Command");
    CHECK(p.window.log.Calls() ==
          std::vector<std::string>{"SetTitle(\"Before\")", "SetTitle(\"Starfleet Command\")"});
}

TEST_CASE("ShowCursor shows the platform cursor while the count is not negative") {
    Plain p;
    CHECK(p.windows.ShowCursor(false) == -1);
    CHECK(p.windows.ShowCursor(false) == -2);
    CHECK(p.windows.ShowCursor(true) == -1);
    CHECK(p.windows.ShowCursor(true) == 0);
    CHECK(p.window.log.Calls() ==
          std::vector<std::string>{"SetCursorVisible(false)", "SetCursorVisible(true)"});
}

TEST_CASE("Invalid regions: a shown window is invalid until validated") {
    Plain p;
    const std::uint32_t hwnd = p.Make(kWsPopup);
    CHECK(p.windows.NextInvalid(0) == 0);
    p.windows.SetVisible(hwnd, true);
    CHECK(p.windows.NextInvalid(0) == hwnd);
    CHECK(p.windows.Find(hwnd)->invalidRect.right == 100);
    p.windows.Validate(hwnd);
    CHECK(p.windows.NextInvalid(0) == 0);
    const Rect small{1, 2, 3, 4};
    p.windows.Invalidate(hwnd, &small);
    const Rect other{0, 5, 2, 6};
    p.windows.Invalidate(hwnd, &other);
    const Rect u = p.windows.Find(hwnd)->invalidRect;
    CHECK(u.left == 0);
    CHECK(u.top == 2);
    CHECK(u.right == 3);
    CHECK(u.bottom == 6);
}

TEST_CASE("Menus, the system menu, and CheckMenuItem's previous state") {
    Plain p;
    const std::uint32_t hwnd = p.Make(kWsPopup);
    const std::uint32_t menu = p.windows.CreateMenu();
    CHECK(p.windows.IsMenu(menu));
    CHECK(p.windows.AppendMenu(menu, 0, 100, "&Debug"));
    CHECK(p.windows.CheckMenuItem(menu, 100, 0x0008) == 0);
    CHECK(p.windows.CheckMenuItem(menu, 100, 0x0000) == 0x0008);
    CHECK(p.windows.CheckMenuItem(menu, 101, 0x0008) == 0xFFFFFFFFu);
    const std::uint32_t system = p.windows.SystemMenu(hwnd);
    CHECK(system == p.windows.SystemMenu(hwnd));
    REQUIRE(p.windows.MenuItems(system) != nullptr);
    CHECK(p.windows.MenuItems(system)->at(0).id == kScClose);
    CHECK(p.windows.DeleteMenu(system, kScClose, 0));
    CHECK(p.windows.MenuItems(system)->empty());
    CHECK_FALSE(p.windows.DeleteMenu(system, kScClose, 0));
}

TEST_CASE("The clipboard keeps data only while open") {
    Plain p;
    CHECK_FALSE(p.windows.SetClipboard(1, {'a'}));
    CHECK(p.windows.OpenClipboard(0));
    CHECK_FALSE(p.windows.OpenClipboard(0));
    CHECK(p.windows.EmptyClipboard());
    CHECK(p.windows.SetClipboard(1, {'h', 'i', 0}));
    REQUIRE(p.windows.Clipboard(1) != nullptr);
    CHECK(p.windows.Clipboard(1)->size() == 3);
    CHECK(p.windows.CloseClipboard());
    CHECK(p.windows.Clipboard(1) == nullptr);
    CHECK_FALSE(p.windows.CloseClipboard());
}

TEST_CASE("Hooks are recorded and removed once") {
    Plain p;
    const std::uint32_t hook = p.windows.AddHook(kWhKeyboardLl, 0x00699660);
    CHECK(hook == 0x5004);
    CHECK(p.windows.HookCount() == 1);
    CHECK(p.windows.RemoveHook(hook));
    CHECK_FALSE(p.windows.RemoveHook(hook));
}

// ---- through User32 ----

TEST_CASE("CreateWindowExA sends the creation messages, then shows and activates") {
    Rig rig;
    const std::uint32_t hwnd = rig.GameWindow();
    CHECK(rig.guest.Messages(hwnd) ==
          std::vector<std::uint32_t>{kWmNcCreate, kWmCreate, kWmSize, kWmMove, kWmShowWindow,
                                     kWmActivateApp, kWmActivate, kWmSetFocus});
    CHECK(rig.guest.calls[0].lParam == 0x00120000u); // the CREATESTRUCTA
    CHECK(rig.guest.calls[2].lParam == ((480u << 16) | 640u));
    CHECK(rig.guest.calls[6].wParam == kWaActive);
    CHECK(rig.user.Windows().Active() == hwnd);
    CHECK(rig.user.Windows().Focus() == hwnd);
}

TEST_CASE("A window whose WM_NCCREATE answers 0, or WM_CREATE -1, is not made") {
    Rig rig;
    rig.guest.procs[kGameProc] = [](const test::Call &call) -> std::uint32_t {
        return call.message == kWmCreate ? 0xFFFFFFFFu : 1u;
    };
    CreateParams params;
    params.className = "GameWindow";
    CHECK(rig.user.CreateWindow(params, 0) == 0);
    CHECK(rig.guest.Messages().back() == kWmNcDestroy);
    CHECK(rig.user.Windows().TopLevelWindows().empty());
}

TEST_CASE("SW_SHOWNOACTIVATE shows a window without taking the activation") {
    Rig rig;
    const std::uint32_t game = rig.GameWindow();
    CreateParams debug;
    debug.className = "GameWindow";
    debug.style = kWsPopup;
    const std::uint32_t second = rig.user.CreateWindow(debug, 0);
    CHECK_FALSE(rig.user.ShowWindow(second, kSwShowNoActivate));
    CHECK(rig.user.Windows().Active() == game);
    CHECK(rig.user.ShowWindow(second, kSwShow));
    CHECK(rig.user.Windows().Active() == second);
}

TEST_CASE("DestroyWindow sends WM_DESTROY parent first and WM_NCDESTROY child first") {
    Rig rig;
    const std::uint32_t parent = rig.GameWindow(kWsPopup);
    CreateParams params;
    params.className = "GameWindow";
    params.style = kWsChild;
    params.parent = parent;
    const std::uint32_t child = rig.user.CreateWindow(params, 0);
    rig.guest.calls.clear();
    CHECK(rig.user.DestroyWindow(parent));
    std::vector<std::pair<std::uint32_t, std::uint32_t>> seen;
    for (const test::Call &call : rig.guest.calls) {
        seen.emplace_back(call.hwnd, call.message);
    }
    CHECK(seen == std::vector<std::pair<std::uint32_t, std::uint32_t>>{{parent, kWmDestroy},
                                                                       {child, kWmDestroy},
                                                                       {child, kWmNcDestroy},
                                                                       {parent, kWmNcDestroy}});
    CHECK_FALSE(rig.user.Windows().IsWindow(child));
}

TEST_CASE("A subclassed window reaches the old procedure through CallWindowProcA") {
    Rig rig;
    const std::uint32_t hwnd = rig.GameWindow();
    const std::uint32_t old = rig.user.SetWindowLong(hwnd, kGwlWndProc, kOtherProc);
    CHECK(old == kGameProc);
    rig.guest.calls.clear();
    rig.guest.procs[kOtherProc] = [&](const test::Call &call) {
        return rig.user.CallProc(old, call.hwnd, call.message, call.wParam, call.lParam) + 1;
    };
    rig.guest.procs[kGameProc] = [](const test::Call &) { return 41u; };
    CHECK(rig.user.Send(hwnd, 0x0400, 1, 2) == 42u);
    REQUIRE(rig.guest.calls.size() == 2);
    CHECK(rig.guest.calls[0].proc == kOtherProc);
    CHECK(rig.guest.calls[1].proc == kGameProc);
}

TEST_CASE("DefWindowProcA: WM_CLOSE destroys, WM_GETTEXT copies, Alt+F4 closes") {
    Rig rig;
    const std::uint32_t hwnd = rig.GameWindow();
    CHECK(rig.user.DefWindowProc(hwnd, kWmGetTextLength, 0, 0) == 4);
    CHECK(rig.user.DefWindowProc(hwnd, kWmGetText, 3, 0x00500000) == 2);
    CHECK(rig.guest.written[0x00500000] == "Ga");
    rig.guest.procs[kGameProc] = [&](const test::Call &call) {
        return rig.user.DefWindowProc(call.hwnd, call.message, call.wParam, call.lParam);
    };
    rig.user.DefWindowProc(hwnd, kWmSysKeyDown, kVkF4, 1u << 29);
    CHECK_FALSE(rig.user.Windows().IsWindow(hwnd));
}

TEST_CASE("GetSystemMetrics speaks the screen and this module's frame") {
    Rig rig;
    CHECK(rig.user.SystemMetric(kSmCxScreen) == 640);
    CHECK(rig.user.SystemMetric(kSmCyScreen) == 480);
    CHECK(rig.user.SystemMetric(kSmCyCaption) == 19);
    CHECK(rig.user.SystemMetric(kSmCxFrame) == 4);
    CHECK(rig.user.SystemMetric(9999) == 0);
}

TEST_CASE("EnumDisplaySettingsA lists each mode at 16 and 32 bits, then ends") {
    Rig rig;
    std::uint32_t bits = 0;
    auto mode = rig.user.DisplayMode(0, bits);
    REQUIRE(mode);
    CHECK(mode->width == 640);
    CHECK(bits == 16);
    mode = rig.user.DisplayMode(3, bits);
    REQUIRE(mode);
    CHECK(mode->width == 800);
    CHECK(bits == 32);
    CHECK_FALSE(rig.user.DisplayMode(14, bits));
    mode = rig.user.DisplayMode(kEnumCurrentSettings, bits);
    REQUIRE(mode);
    CHECK(mode->height == 480);
    CHECK(bits == 32);
}

} // namespace sfcop::hle::user32
