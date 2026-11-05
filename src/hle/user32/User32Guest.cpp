// The guest shims of the user32 module (src/hle/README.md, "Implementing a module"): the USER32
// and COMDLG32 rows of src/hle/surface.tsv. Each reads its arguments from the guest stack, reads
// and writes the game's structures (MSG, RECT, POINT, WNDCLASSA, CREATESTRUCTA, PAINTSTRUCT,
// DEVMODEA) field by field, calls the module (User32.h) and purges the row's stack bytes.
//
// Windows, menus and hooks are entries of win32hle's handle table of this module's types (27,
// 28, 29; src/hle/README.md, "Handle types"). The game's window, dialog and hook procedures are
// called with hle_call_guest; a wait (GetMessageA, a modal dialog) gives the machine up between
// pumps. A FillRect or ReleaseDC on a DC the gdi32 module did not make runs win32hle's own row.
//
// COMDLG32: the game never opens a file dialog on its way to the menus; the four dialogs answer
// as if the user cancelled, and CommDlgExtendedError says no error.
#include "hle/gdi32/GuestGdi.h"
#include "hle/user32/Dialog.h"
#include "hle/user32/Keys.h"
#include "hle/user32/User32.h"
#include "hle/user32/User32Defs.h"
#include "hle/user32/User32Marshal.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace sfcop::hle::user32 {

namespace {

using gdi32::guest::foreignDc;
using gdi32::guest::gdi;

constexpr int kWindowHandleType = 27;
constexpr int kMenuHandleType = 28;
constexpr int kHookHandleType = 29;

// ---- guest memory ----

// The structures are read and written by User32Marshal.h on the guest's bytes; these are the guest
// address to bytes, and the null-checked forms the rows use.
std::uint8_t *at(std::uint32_t address) {
    return static_cast<std::uint8_t *>(hle_ptr(address));
}

void put32(std::uint32_t address, std::uint32_t value) {
    if (address != 0) {
        marshal::put32(at(address), value);
    }
}

std::int32_t s32(std::uint32_t value) {
    return static_cast<std::int32_t>(value);
}

// The longest strings the rows read: a class name (the Win32 limit), a window's or menu item's text,
// a format string (wsprintfA's own output limit).
constexpr std::size_t kMaxClassName = 256;
constexpr std::size_t kMaxText = 1024;

// A zero-terminated string of the game's, read no further than `limit` characters.
std::string guestString(std::uint32_t address, std::size_t limit = kMaxText) {
    if (address == 0) {
        return {};
    }
    const char *text = static_cast<const char *>(hle_ptr(address));
    return std::string(text, strnlen(text, limit));
}

// A class or resource name: a string, or an atom below 0x10000 given as "#<n>".
std::string nameOrAtom(std::uint32_t address) {
    if (address != 0 && address < 0x10000u) {
        return "#" + std::to_string(address);
    }
    return guestString(address, kMaxClassName);
}

Rect readRect(std::uint32_t address) {
    return marshal::readRect(at(address));
}

void writeRect(std::uint32_t address, const Rect &r) {
    if (address != 0) {
        marshal::writeRect(at(address), r);
    }
}

void writeMessage(std::uint32_t address, const Message &m) {
    if (address != 0) {
        marshal::writeMessage(at(address), m);
    }
}

Message readMessage(std::uint32_t address) {
    return marshal::readMessage(at(address));
}

// A SHORT key state in EAX, sign-extended as the game's callers read it.
std::uint32_t shortResult(std::uint16_t value) {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(value)));
}

void handOn(const char *name, std::uint32_t argc, std::uint32_t failure) {
    if (recomp_func_t row = gdi32::guest::win32hleRow(name)) {
        row();
        return;
    }
    RET(failure, argc);
}

// ---- the game, as User32 reaches it ----

class Guest final : public GuestCalls {
public:
    std::uint32_t CallProc(std::uint32_t proc, std::uint32_t hwnd, std::uint32_t message,
                           std::uint32_t wParam, std::uint32_t lParam) override {
        const std::uint32_t args[] = {hwnd, message, wParam, lParam};
        return hle_call_guest(proc, 4, args);
    }
    std::uint32_t WriteText(std::uint32_t address, std::uint32_t size,
                            const std::string &text) override {
        if (address == 0 || size == 0) {
            return 0;
        }
        const std::uint32_t n =
            static_cast<std::uint32_t>(std::min<std::size_t>(text.size(), size - 1));
        std::memcpy(hle_ptr(address), text.data(), n);
        MEM8(address + n) = 0;
        return n;
    }
    void Idle(std::uint32_t ms) override {
        hle_block_begin();
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        hle_block_end();
    }
};

// The first row the game calls makes the module. The host must have attached before any guest code
// runs; a row that finds none is an error: it is logged and the run fails (hle_exit never returns
// to the row that asked, so the fallthrough below is unreachable in a real run).
User32 &user() {
    static Guest guest;
    static User32 *made = [] {
        const Host *attachedHost = AttachedHost();
        if (attachedHost == nullptr) {
            std::fprintf(stderr, "[user32] error: a row ran before AttachHost\n");
            hle_exit(1);
            std::abort();
        }
        const Host &host = *attachedHost;
        WindowHost handles{
            [] { return gdi32::guest::screen(); },
            [](UserHandle kind) {
                const int type = kind == UserHandle::kWindow ? kWindowHandleType
                                 : kind == UserHandle::kMenu ? kMenuHandleType
                                                             : kHookHandleType;
                return hle_handle_alloc(type, &gdi32::guest::g_handleMarker);
            },
            [](std::uint32_t handle) { hle_handle_close(handle); },
            [] { return OnMainThread(); }};
        auto *created = new User32(*host.window, *host.input, gdi(), guest, std::move(handles),
                                   [] { return hle_ticks_ms(); });
        created->SetHostPump(host.pump);
        created->SetStopping(host.stopping);
        if (host.gpu != nullptr && host.display) {
            created->SetPresenter(makeGpuPresenter(*host.gpu, host.display));
        }
        // The pump's events from here on (and those it kept so far) reach the queue.
        SetEventSink([created](const platform::WindowEvent &event) {
            created->OnWindowEvent(event);
        });
        return created;
    }();
    return *made;
}

WindowManager &windows() {
    return user().Windows();
}

// ---- classes and windows ----

void u_RegisterClassA() { // (const WNDCLASSA *)
    const std::uint32_t c = A32(0);
    if (c == 0) {
        RET(0, 1);
    }
    const marshal::WndClassFields fields = marshal::readWndClass(at(c));
    WindowClass made;
    made.style = fields.style;
    made.proc = fields.proc;
    made.classExtra = fields.classExtra;
    made.windowExtra = fields.windowExtra;
    made.instance = fields.instance;
    made.icon = fields.icon;
    made.cursor = fields.cursor;
    made.background = fields.background;
    made.name = nameOrAtom(fields.className);
    RET(user().RegisterClass(made), 1);
}

// (exStyle, className, windowName, style, x, y, width, height, parent, menu, instance, param)
void u_CreateWindowExA() {
    CreateParams params;
    params.exStyle = A32(0);
    params.className = nameOrAtom(A32(1));
    params.windowName = guestString(A32(2));
    params.style = A32(3);
    params.x = s32(A32(4)), params.y = s32(A32(5));
    params.width = s32(A32(6)), params.height = s32(A32(7));
    params.parent = A32(8);
    params.menuOrId = A32(9);
    params.instance = A32(10);
    // CREATESTRUCTA, which WM_NCCREATE and WM_CREATE carry.
    const std::uint32_t mark = hle_scratch_mark();
    const std::uint32_t cs = hle_scratch(48);
    const std::uint32_t fields[] = {A32(11), A32(10), A32(9), A32(8), A32(7), A32(6),
                                    A32(5),  A32(4),  A32(3), A32(2), A32(1), A32(0)};
    for (std::uint32_t i = 0; i < 12; ++i) {
        put32(cs + 4 * i, fields[i]);
    }
    const std::uint32_t hwnd = user().CreateWindow(params, cs);
    hle_scratch_release(mark);
    RET(hwnd, 12);
}

void u_DestroyWindow() { // (hwnd)
    RET(user().DestroyWindow(A32(0)) ? 1 : 0, 1);
}

void u_ShowWindow() { // (hwnd, command)
    RET(user().ShowWindow(A32(0), A32(1)) ? 1 : 0, 2);
}

void u_UpdateWindow() { // (hwnd): WM_PAINT now, if the window is invalid
    WindowRecord *record = windows().Find(A32(0));
    if (record == nullptr) {
        RET(0, 1);
    }
    if (record->invalid && record->visible) {
        user().Send(A32(0), kWmPaint, 0, 0);
    }
    RET(1, 1);
}

void u_RedrawWindow() { // (hwnd, const RECT *, region, flags)
    constexpr std::uint32_t kRdwInvalidate = 0x0001, kRdwUpdateNow = 0x0100;
    const std::uint32_t hwnd = A32(0);
    if (!windows().IsWindow(hwnd)) {
        RET(0, 4);
    }
    if ((A32(3) & kRdwInvalidate) != 0) {
        const Rect rect = A32(1) != 0 ? readRect(A32(1)) : Rect{};
        windows().Invalidate(hwnd, A32(1) != 0 ? &rect : nullptr);
    }
    if ((A32(3) & kRdwUpdateNow) != 0 && windows().Find(hwnd)->invalid) {
        user().Send(hwnd, kWmPaint, 0, 0);
    }
    RET(1, 4);
}

void u_InvalidateRect() { // (hwnd, const RECT *, erase)
    const std::uint32_t hwnd = A32(0);
    if (hwnd == 0) {
        for (const std::uint32_t top : windows().TopLevelWindows()) {
            windows().Invalidate(top, nullptr);
        }
        RET(1, 3);
    }
    if (!windows().IsWindow(hwnd)) {
        RET(0, 3);
    }
    const Rect rect = A32(1) != 0 ? readRect(A32(1)) : Rect{};
    windows().Invalidate(hwnd, A32(1) != 0 ? &rect : nullptr);
    RET(1, 3);
}

void u_GetUpdateRect() { // (hwnd, RECT *, erase)
    const WindowRecord *record = windows().Find(A32(0));
    if (record == nullptr) {
        RET(0, 3);
    }
    if (A32(1) != 0) {
        writeRect(A32(1), record->invalid ? record->invalidRect : Rect{});
    }
    RET(record->invalid ? 1 : 0, 3);
}

void u_IsWindow() { // (hwnd)
    RET(windows().IsWindow(A32(0)) ? 1 : 0, 1);
}

void u_IsIconic() { // (hwnd)
    RET(windows().IsIconic(A32(0)) ? 1 : 0, 1);
}

void u_GetActiveWindow() {
    RET(windows().Active(), 0);
}

void u_SetActiveWindow() { // (hwnd)
    RET(user().SetActiveWindow(A32(0)), 1);
}

void u_SetForegroundWindow() { // (hwnd)
    if (!windows().IsWindow(A32(0))) {
        RET(0, 1);
    }
    user().SetActiveWindow(A32(0));
    RET(1, 1);
}

void u_SetFocus() { // (hwnd)
    RET(user().SetFocus(A32(0)), 1);
}

void u_GetDesktopWindow() {
    RET(kDesktopWindow, 0);
}

void u_FindWindowA() { // (className, windowName)
    RET(windows().FindTopLevel(nameOrAtom(A32(0)), guestString(A32(1))), 2);
}

void u_SetWindowTextA() { // (hwnd, text)
    RET(user().SetWindowText(A32(0), guestString(A32(1))) ? 1 : 0, 2);
}

void u_GetWindowLongA() { // (hwnd, index)
    RET(windows().GetLong(A32(0), s32(A32(1))).value_or(0), 2);
}

void u_SetWindowLongA() { // (hwnd, index, value)
    RET(user().SetWindowLong(A32(0), s32(A32(1)), A32(2)), 3);
}

void u_SetWindowPos() { // (hwnd, insertAfter, x, y, width, height, flags)
    RET(user().SetWindowPos(A32(0), s32(A32(2)), s32(A32(3)), s32(A32(4)), s32(A32(5)), A32(6))
            ? 1
            : 0,
        7);
}

// ---- geometry ----

Rect screenRect() {
    const gdi32::ScreenInfo screen = windows().Screen();
    return Rect{0, 0, screen.width, screen.height};
}

void u_GetClientRect() { // (hwnd, RECT *)
    if (A32(0) == kDesktopWindow) {
        writeRect(A32(1), screenRect());
        RET(1, 2);
    }
    if (!windows().IsWindow(A32(0)) || A32(1) == 0) {
        RET(0, 2);
    }
    writeRect(A32(1), windows().ClientRect(A32(0)));
    RET(1, 2);
}

void u_GetWindowRect() { // (hwnd, RECT *)
    if (A32(0) == kDesktopWindow) {
        writeRect(A32(1), screenRect());
        RET(1, 2);
    }
    if (!windows().IsWindow(A32(0)) || A32(1) == 0) {
        RET(0, 2);
    }
    writeRect(A32(1), windows().WindowRect(A32(0)));
    RET(1, 2);
}

void u_AdjustWindowRect() { // (RECT *, style, menu)
    if (A32(0) == 0) {
        RET(0, 3);
    }
    writeRect(A32(0), WindowManager::AdjustRect(readRect(A32(0)), A32(1), A32(2) != 0));
    RET(1, 3);
}

// ClientToScreen (sign +1) and ScreenToClient (sign -1): the point moves by the client origin.
void moveByOrigin(int sign) { // (hwnd, POINT *)
    if (A32(1) == 0) {
        RET(0, 2);
    }
    std::int32_t x = 0, y = 0;
    if (A32(0) != kDesktopWindow) {
        if (!windows().IsWindow(A32(0))) {
            RET(0, 2);
        }
        windows().ClientOrigin(A32(0), x, y);
    }
    marshal::Point point = marshal::readPoint(at(A32(1)));
    point.x += sign * x;
    point.y += sign * y;
    marshal::writePoint(at(A32(1)), point);
    RET(1, 2);
}

void u_ClientToScreen() {
    moveByOrigin(1);
}

void u_ScreenToClient() {
    moveByOrigin(-1);
}

void u_SetRect() { // (RECT *, left, top, right, bottom)
    if (A32(0) == 0) {
        RET(0, 5);
    }
    writeRect(A32(0), Rect{s32(A32(1)), s32(A32(2)), s32(A32(3)), s32(A32(4))});
    RET(1, 5);
}

void u_OffsetRect() { // (RECT *, dx, dy)
    if (A32(0) == 0) {
        RET(0, 3);
    }
    Rect r = readRect(A32(0));
    r.left += s32(A32(1)), r.right += s32(A32(1));
    r.top += s32(A32(2)), r.bottom += s32(A32(2));
    writeRect(A32(0), r);
    RET(1, 3);
}

// ---- messages ----

void u_PeekMessageA() { // (MSG *, hwnd, first, last, remove)
    const Retrieved got = user().Peek(A32(1), A32(2), A32(3), (A32(4) & kPmRemove) != 0);
    if (got.found) {
        writeMessage(A32(0), got.message);
    }
    RET(got.found ? 1 : 0, 5);
}

void u_GetMessageA() { // (MSG *, hwnd, first, last)
    const Message got = user().Get(A32(1), A32(2), A32(3));
    writeMessage(A32(0), got);
    RET(got.message == kWmQuit ? 0 : 1, 4);
}

void u_TranslateMessage() { // (const MSG *)
    RET(A32(0) != 0 && user().Translate(readMessage(A32(0))) ? 1 : 0, 1);
}

void u_DispatchMessageA() { // (const MSG *)
    RET(A32(0) != 0 ? user().Dispatch(readMessage(A32(0))) : 0, 1);
}

void u_SendMessageA() { // (hwnd, message, wParam, lParam)
    RET(user().Send(A32(0), A32(1), A32(2), A32(3)), 4);
}

void u_PostMessageA() { // (hwnd, message, wParam, lParam)
    RET(user().Post(A32(0), A32(1), A32(2), A32(3)) ? 1 : 0, 4);
}

void u_PostQuitMessage() { // (code)
    user().Queue().PostQuit(s32(A32(0)));
    RETV(1);
}

void u_DefWindowProcA() { // (hwnd, message, wParam, lParam)
    RET(user().DefWindowProc(A32(0), A32(1), A32(2), A32(3)), 4);
}

void u_CallWindowProcA() { // (proc, hwnd, message, wParam, lParam)
    RET(user().CallProc(A32(0), A32(1), A32(2), A32(3), A32(4)), 5);
}

// ---- dialogs ----

void u_DialogBoxIndirectParamA() { // (instance, const DLGTEMPLATE *, owner, dialogProc, param)
    // The template's own item count and layout end the read.
    if (A32(1) == 0) {
        RET(0xFFFFFFFFu, 5);
    }
    const std::optional<DialogTemplate> dialog = parseDialogTemplate(at(A32(1)));
    if (!dialog) {
        RET(0xFFFFFFFFu, 5);
    }
    RET(static_cast<std::uint32_t>(user().DialogBox(*dialog, A32(2), A32(0), A32(3), A32(4))), 5);
}

void u_EndDialog() { // (hwnd, result)
    RET(user().EndDialog(A32(0), s32(A32(1))) ? 1 : 0, 2);
}

void u_MessageBoxA() { // (owner, text, caption, type)
    RET(static_cast<std::uint32_t>(
            user().MessageBox(A32(0), guestString(A32(1)), guestString(A32(2)), A32(3))),
        4);
}

void u_MessageBeep() { // (type)
    RET(1, 1);
}

// ---- painting ----

void u_BeginPaint() { // (hwnd, PAINTSTRUCT *)
    Rect paint;
    bool erase = false;
    const std::uint32_t dc = user().BeginPaint(A32(0), paint, erase);
    if (A32(1) != 0) {
        marshal::writePaintStruct(at(A32(1)), dc, erase, paint);
    }
    RET(dc, 2);
}

void u_EndPaint() { // (hwnd, const PAINTSTRUCT *)
    if (A32(1) != 0) {
        user().EndPaint(A32(0), marshal::paintStructDc(at(A32(1))));
    }
    RET(1, 2);
}

void u_GetDC() { // (hwnd)
    RET(user().GetDC(A32(0)), 1);
}

void u_ReleaseDC() { // (hwnd, dc)
    if (foreignDc(A32(1))) {
        return handOn("ReleaseDC", 2, 0);
    }
    RET(user().ReleaseDC(A32(0), A32(1)) ? 1 : 0, 2);
}

void u_FillRect() { // (dc, const RECT *, brush)
    if (foreignDc(A32(0))) {
        return handOn("FillRect", 3, 0);
    }
    if (A32(1) == 0) {
        RET(0, 3);
    }
    RET(gdi().FillRect(A32(0), readRect(A32(1)), A32(2)) ? 1 : 0, 3);
}

// ---- input ----

void u_GetCursorPos() { // (POINT *)
    std::int32_t x = 0, y = 0;
    user().CursorPos(x, y);
    if (A32(0) == 0) {
        RET(0, 1);
    }
    marshal::writePoint(at(A32(0)), marshal::Point{x, y});
    RET(1, 1);
}

void u_GetAsyncKeyState() { // (vk)
    RET(shortResult(user().AsyncKeyState(A32(0))), 1);
}

void u_GetKeyState() { // (vk)
    RET(shortResult(user().KeyState(A32(0))), 1);
}

void u_MapVirtualKeyA() { // (code, mapType)
    RET(mapVirtualKey(A32(0), A32(1)), 2);
}

void u_VkKeyScanA() { // (character)
    RET(shortResult(static_cast<std::uint16_t>(vkKeyScan(static_cast<char>(A32(0))))), 1);
}

void u_ShowCursor() { // (show)
    RET(static_cast<std::uint32_t>(windows().ShowCursor(A32(0) != 0)), 1);
}

void u_LoadCursorA() { // (instance, name)
    // A system cursor (IDC_*) or one of the game's: a handle the game only hands back.
    const std::uint32_t name = A32(1);
    RET(kCursorBase | (name < 0x10000u ? name : 1u), 2);
}

void u_SetCursor() { // (cursor)
    RET(windows().SetCursor(A32(0)), 1);
}

// ---- hooks ----

void u_SetWindowsHookExA() { // (id, proc, module, threadId)
    // Recorded, never called: the game's one hook (a low-level keyboard hook) only keeps the
    // system's task-switch keys from Windows, which the platform window does not pass on.
    RET(windows().AddHook(s32(A32(0)), A32(1)), 4);
}

void u_UnhookWindowsHookEx() { // (hook)
    RET(windows().RemoveHook(A32(0)) ? 1 : 0, 1);
}

void u_CallNextHookEx() { // (hook, code, wParam, lParam)
    RET(0, 4);
}

// ---- menus ----

void u_LoadMenuA() { // (instance, name): the menu as bookkeeping, without the resource's items
    RET(windows().CreateMenu(), 2);
}

void u_GetMenu() { // (hwnd)
    const WindowRecord *record = windows().Find(A32(0));
    RET(record != nullptr && (record->style & kWsChild) == 0 ? record->id : 0, 1);
}

void u_SetMenu() { // (hwnd, menu)
    WindowRecord *record = windows().Find(A32(0));
    if (record == nullptr || (record->style & kWsChild) != 0 ||
        (A32(1) != 0 && !windows().IsMenu(A32(1)))) {
        RET(0, 2);
    }
    record->id = A32(1);
    RET(1, 2);
}

void u_DrawMenuBar() { // (hwnd)
    RET(windows().IsWindow(A32(0)) ? 1 : 0, 1);
}

void u_AppendMenuA() { // (menu, flags, id, item)
    constexpr std::uint32_t kNotText = 0x0004 | 0x0100 | 0x0800; // MF_BITMAP, OWNERDRAW, SEPARATOR
    const std::string text = (A32(1) & kNotText) == 0 ? guestString(A32(3)) : std::string();
    RET(windows().AppendMenu(A32(0), A32(1), A32(2), text) ? 1 : 0, 4);
}

void u_CheckMenuItem() { // (menu, item, flags)
    RET(windows().CheckMenuItem(A32(0), A32(1), A32(2)), 3);
}

void u_DeleteMenu() { // (menu, item, flags)
    RET(windows().DeleteMenu(A32(0), A32(1), A32(2)) ? 1 : 0, 3);
}

void u_GetSystemMenu() { // (hwnd, revert)
    RET(A32(1) != 0 ? 0 : windows().SystemMenu(A32(0)), 2);
}

// ---- the clipboard ----

void u_OpenClipboard() { // (owner)
    RET(windows().OpenClipboard(A32(0)) ? 1 : 0, 1);
}

void u_CloseClipboard() {
    RET(windows().CloseClipboard() ? 1 : 0, 0);
}

void u_EmptyClipboard() {
    RET(windows().EmptyClipboard() ? 1 : 0, 0);
}

// The clipboard keeps the game's memory handle; the game reads its data back through it.
void u_SetClipboardData() { // (format, memory)
    const std::uint32_t memory = A32(1);
    const std::vector<std::uint8_t> handle{
        static_cast<std::uint8_t>(memory), static_cast<std::uint8_t>(memory >> 8),
        static_cast<std::uint8_t>(memory >> 16), static_cast<std::uint8_t>(memory >> 24)};
    RET(windows().SetClipboard(A32(0), handle) ? memory : 0, 2);
}

void u_GetClipboardData() { // (format)
    const std::vector<std::uint8_t> *data = windows().Clipboard(A32(0));
    if (data == nullptr || data->size() != 4) {
        RET(0, 1);
    }
    RET(static_cast<std::uint32_t>((*data)[0]) | static_cast<std::uint32_t>((*data)[1]) << 8 |
            static_cast<std::uint32_t>((*data)[2]) << 16 |
            static_cast<std::uint32_t>((*data)[3]) << 24,
        1);
}

// ---- the screen and the system ----

void u_GetSystemMetrics() { // (index)
    RET(static_cast<std::uint32_t>(user().SystemMetric(A32(0))), 1);
}

void u_SystemParametersInfoA() { // (action, uiParam, pvParam, winIni)
    // The game's two calls are SPI_SCREENSAVERRUNNING (0x61); an action not answered fails with
    // ERROR_INVALID_PARAMETER, writing nothing.
    const marshal::SpiAnswer answer = marshal::systemParametersInfo(A32(0), screenRect());
    if (!answer.handled) {
        hle_set_last_error(kErrorInvalidParameter);
        RET(0, 4);
    }
    if (A32(2) != 0) {
        for (std::uint32_t i = 0; i < answer.words; ++i) {
            put32(A32(2) + 4 * i, answer.value[i]);
        }
    }
    RET(1, 4);
}

void u_EnumDisplaySettingsA() { // (deviceName, modeIndex, DEVMODEA *)
    std::uint32_t bits = 0;
    const std::optional<platform::DisplayMode> mode = user().DisplayMode(A32(1), bits);
    if (!mode || A32(2) == 0) {
        RET(0, 3);
    }
    marshal::writeDevMode(at(A32(2)), bits, mode->width, mode->height, mode->refreshHz);
    RET(1, 3);
}

void u_ChangeDisplaySettingsA() { // (const DEVMODEA *, flags)
    // The display mode is DirectDraw's to set (hle/ddraw4); the request is accepted unchanged.
    RET(kDispChangeSuccessful, 2);
}

// wsprintfA (buffer, format, ...): cdecl, the caller purges.
void u_wsprintfA() {
    std::uint32_t arguments = g_esp + 4u + 8u; // after the return address, buffer and format
    const std::string text = marshal::formatWsprintf(
        guestString(A32(1), marshal::kWsprintfLimit),
        [&arguments] {
            const std::uint32_t word = MEM32(arguments);
            arguments += 4;
            return word;
        },
        [](std::uint32_t address) {
            return address != 0 ? guestString(address, marshal::kWsprintfLimit) : "(null)";
        });
    if (A32(0) != 0) {
        std::memcpy(hle_ptr(A32(0)), text.data(), text.size());
        MEM8(A32(0) + text.size()) = 0;
    }
    RET(text.size(), 0);
}

// ---- COMDLG32 ----

void c_GetFileName() { // (OPENFILENAME *): cancelled
    RET(0, 1);
}

void c_CommDlgExtendedError() {
    RET(0, 0);
}

} // namespace

} // namespace sfcop::hle::user32

namespace u = sfcop::hle::user32;

extern "C" const win32hle_shim sfcop_hle_user32[] = {
    // classes and windows
    {"RegisterClassA", u::u_RegisterClassA},
    {"CreateWindowExA", u::u_CreateWindowExA},
    {"DestroyWindow", u::u_DestroyWindow},
    {"ShowWindow", u::u_ShowWindow},
    {"UpdateWindow", u::u_UpdateWindow},
    {"RedrawWindow", u::u_RedrawWindow},
    {"InvalidateRect", u::u_InvalidateRect},
    {"GetUpdateRect", u::u_GetUpdateRect},
    {"IsWindow", u::u_IsWindow},
    {"IsIconic", u::u_IsIconic},
    {"GetActiveWindow", u::u_GetActiveWindow},
    {"SetActiveWindow", u::u_SetActiveWindow},
    {"SetForegroundWindow", u::u_SetForegroundWindow},
    {"SetFocus", u::u_SetFocus},
    {"GetDesktopWindow", u::u_GetDesktopWindow},
    {"FindWindowA", u::u_FindWindowA},
    {"SetWindowTextA", u::u_SetWindowTextA},
    {"GetWindowLongA", u::u_GetWindowLongA},
    {"SetWindowLongA", u::u_SetWindowLongA},
    {"SetWindowPos", u::u_SetWindowPos},
    // geometry
    {"GetClientRect", u::u_GetClientRect},
    {"GetWindowRect", u::u_GetWindowRect},
    {"AdjustWindowRect", u::u_AdjustWindowRect},
    {"ClientToScreen", u::u_ClientToScreen},
    {"ScreenToClient", u::u_ScreenToClient},
    {"SetRect", u::u_SetRect},
    {"OffsetRect", u::u_OffsetRect},
    // messages
    {"PeekMessageA", u::u_PeekMessageA},
    {"GetMessageA", u::u_GetMessageA},
    {"TranslateMessage", u::u_TranslateMessage},
    {"DispatchMessageA", u::u_DispatchMessageA},
    {"SendMessageA", u::u_SendMessageA},
    {"PostMessageA", u::u_PostMessageA},
    {"PostQuitMessage", u::u_PostQuitMessage},
    {"DefWindowProcA", u::u_DefWindowProcA},
    {"CallWindowProcA", u::u_CallWindowProcA},
    // dialogs
    {"DialogBoxIndirectParamA", u::u_DialogBoxIndirectParamA},
    {"EndDialog", u::u_EndDialog},
    {"MessageBoxA", u::u_MessageBoxA},
    {"MessageBeep", u::u_MessageBeep},
    // painting
    {"BeginPaint", u::u_BeginPaint},
    {"EndPaint", u::u_EndPaint},
    {"GetDC", u::u_GetDC},
    {"ReleaseDC", u::u_ReleaseDC},
    {"FillRect", u::u_FillRect},
    // input
    {"GetCursorPos", u::u_GetCursorPos},
    {"GetAsyncKeyState", u::u_GetAsyncKeyState},
    {"GetKeyState", u::u_GetKeyState},
    {"MapVirtualKeyA", u::u_MapVirtualKeyA},
    {"VkKeyScanA", u::u_VkKeyScanA},
    {"ShowCursor", u::u_ShowCursor},
    {"LoadCursorA", u::u_LoadCursorA},
    {"SetCursor", u::u_SetCursor},
    // hooks
    {"SetWindowsHookExA", u::u_SetWindowsHookExA},
    {"UnhookWindowsHookEx", u::u_UnhookWindowsHookEx},
    {"CallNextHookEx", u::u_CallNextHookEx},
    // menus
    {"LoadMenuA", u::u_LoadMenuA},
    {"GetMenu", u::u_GetMenu},
    {"SetMenu", u::u_SetMenu},
    {"DrawMenuBar", u::u_DrawMenuBar},
    {"AppendMenuA", u::u_AppendMenuA},
    {"CheckMenuItem", u::u_CheckMenuItem},
    {"DeleteMenu", u::u_DeleteMenu},
    {"GetSystemMenu", u::u_GetSystemMenu},
    // the clipboard
    {"OpenClipboard", u::u_OpenClipboard},
    {"CloseClipboard", u::u_CloseClipboard},
    {"EmptyClipboard", u::u_EmptyClipboard},
    {"SetClipboardData", u::u_SetClipboardData},
    {"GetClipboardData", u::u_GetClipboardData},
    // the screen and the system
    {"GetSystemMetrics", u::u_GetSystemMetrics},
    {"SystemParametersInfoA", u::u_SystemParametersInfoA},
    {"EnumDisplaySettingsA", u::u_EnumDisplaySettingsA},
    {"ChangeDisplaySettingsA", u::u_ChangeDisplaySettingsA},
    {"wsprintfA", u::u_wsprintfA},
    // COMDLG32
    {"GetOpenFileNameA", u::c_GetFileName},
    {"GetOpenFileNameW", u::c_GetFileName},
    {"GetSaveFileNameA", u::c_GetFileName},
    {"GetSaveFileNameW", u::c_GetFileName},
    {"CommDlgExtendedError", u::c_CommDlgExtendedError},
    {nullptr, nullptr},
};
