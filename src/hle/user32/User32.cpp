#include "hle/user32/User32.h"

#include "hle/gdi32/Bitmap.h"
#include "hle/user32/User32Defs.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace sfcop::hle::user32 {

namespace {

constexpr std::uint32_t kAnyWindow = 0;
constexpr std::uint32_t kSizeRestored = 0;
constexpr std::uint32_t kVkF4Key = kVkF4;
constexpr std::uint32_t kMaxRetryAnswers = 3; // MessageBoxA's Retry, answered with nobody to see
constexpr std::uint32_t kWaitMs = 5; // GetMessageA's and a dialog's sleep between pumps

std::uint32_t pack(std::int32_t low, std::int32_t high) {
    return (static_cast<std::uint32_t>(low) & 0xFFFFu) | static_cast<std::uint32_t>(high) << 16;
}

// The modes EnumDisplaySettingsA lists, each at 16 and 32 bits: the ones the game offers.
constexpr struct {
    std::uint32_t width, height;
} kModes[] = {{640, 480}, {800, 600}, {1024, 768}, {1152, 864}, {1280, 960}, {1280, 1024},
              {1600, 1200}};

} // namespace

User32::User32(platform::Window &window, platform::Input &input, gdi32::Gdi &gdi,
               GuestCalls &guest, WindowHost handles, std::function<std::uint32_t()> now)
    : window_(window), input_(input), gdi_(gdi), guest_(guest),
      windows_(window, std::move(handles)), queue_(windows_, std::move(now)) {}

void User32::Pump() {
    if (!windows_.OnMainThread()) {
        return;
    }
    if (hostPump_) {
        hostPump_();
    }
}

void User32::OnWindowEvent(const platform::WindowEvent &event) {
    if (const auto state = window_.GetState(); state.Ok()) {
        hostSize_ = HostSize{state.Value().width, state.Value().height};
    }
    queue_.Feed(event, hostSize_);
}

// ---- classes and windows ----

std::uint16_t User32::RegisterClass(const WindowClass &windowClass) {
    return windows_.RegisterClass(windowClass);
}

std::uint32_t User32::CreateWindow(const CreateParams &params, std::uint32_t createStruct) {
    const std::uint32_t hwnd = windows_.Create(params);
    if (hwnd == 0) {
        return 0;
    }
    if (Send(hwnd, kWmNcCreate, 0, createStruct) == 0) {
        windows_.Destroy(hwnd);
        return 0;
    }
    if (static_cast<std::int32_t>(Send(hwnd, kWmCreate, 0, createStruct)) == -1) {
        DestroyWindow(hwnd);
        return 0;
    }
    if (!windows_.IsWindow(hwnd)) {
        return 0; // the window destroyed itself while being made
    }
    const Rect client = windows_.ClientRect(hwnd);
    Send(hwnd, kWmSize, kSizeRestored, pack(client.right, client.bottom));
    const Rect rect = windows_.Find(hwnd)->rect;
    const Frame frame = frameOf(params.style, params.exStyle);
    Send(hwnd, kWmMove, 0, pack(rect.left + frame.side, rect.top + frame.top));
    if ((params.style & kWsVisible) != 0 && windows_.IsWindow(hwnd)) {
        ShowWindow(hwnd, kSwShow);
    }
    if (!windows_.IsWindow(hwnd)) {
        return 0;
    }
    if (queue_.TakeEarlyClose()) {
        Post(hwnd, kWmClose, 0, 0); // the close box was pressed before any window existed
    }
    return hwnd;
}

bool User32::DestroyWindow(std::uint32_t hwnd) {
    WindowRecord *record = windows_.Find(hwnd);
    if (record == nullptr || record->destroying) {
        return record != nullptr;
    }
    const bool wasActive = windows_.Active() == hwnd;
    if (record->visible) {
        record->visible = false;
        Send(hwnd, kWmShowWindow, 0, 0);
    }
    if (wasActive) {
        Send(hwnd, kWmActivate, kWaInactive, 0);
        Send(hwnd, kWmActivateApp, 0, 0);
    }
    // WM_DESTROY goes to the window, then its children; WM_NCDESTROY last.
    std::vector<std::uint32_t> order{hwnd};
    for (std::size_t i = 0; i < order.size(); ++i) {
        for (const std::uint32_t child : windows_.Children(order[i])) {
            order.push_back(child);
        }
    }
    for (const std::uint32_t each : order) {
        if (WindowRecord *r = windows_.Find(each)) {
            r->destroying = true;
        }
    }
    for (const std::uint32_t each : order) {
        Send(each, kWmDestroy, 0, 0);
    }
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        Send(*it, kWmNcDestroy, 0, 0);
    }
    for (const std::uint32_t removed : windows_.Destroy(hwnd)) {
        queue_.Forget(removed);
    }
    return true;
}

bool User32::ShowWindow(std::uint32_t hwnd, std::uint32_t command) {
    WindowRecord *record = windows_.Find(hwnd);
    if (record == nullptr) {
        return false;
    }
    const bool show = command != kSwHide;
    const bool was = record->visible;
    if (show != was) {
        windows_.SetVisible(hwnd, show);
        Send(hwnd, kWmShowWindow, show ? 1 : 0, 0);
    }
    const bool activates = show && command != kSwShowNoActivate && command != kSwShowNa &&
                           command != kSwShowMinNoActive && command != kSwShowMinimized &&
                           command != kSwMinimize;
    if (activates && (record->style & kWsChild) == 0) {
        activate(hwnd);
    }
    if (!show && windows_.Active() == hwnd) {
        Send(hwnd, kWmActivate, kWaInactive, 0);
        windows_.SetActive(0);
    }
    return was;
}

void User32::activate(std::uint32_t hwnd) {
    const std::uint32_t top = windows_.TopLevelOf(hwnd);
    const std::uint32_t previous = windows_.Active();
    if (top == 0 || top == previous) {
        return;
    }
    if (previous != 0) {
        Send(previous, kWmActivate, kWaInactive, top);
    } else {
        Send(top, kWmActivateApp, 1, 0);
    }
    windows_.SetActive(top);
    // The clicks the queue held while no window was active came before this window: Windows
    // would not have delivered them to it.
    queue_.ClearPending();
    Send(top, kWmActivate, kWaActive, previous);
    if (windows_.IsWindow(top) && windows_.TopLevelOf(windows_.Focus()) != top) {
        SetFocus(top);
    }
}

std::uint32_t User32::SetActiveWindow(std::uint32_t hwnd) {
    const std::uint32_t previous = windows_.Active();
    if (windows_.IsWindow(hwnd)) {
        activate(hwnd);
    }
    return previous;
}

std::uint32_t User32::SetFocus(std::uint32_t hwnd) {
    const std::uint32_t previous = windows_.Focus();
    if (hwnd != 0 && !windows_.IsWindow(hwnd)) {
        return 0;
    }
    if (hwnd == previous) {
        return previous;
    }
    if (previous != 0 && windows_.IsWindow(previous)) {
        Send(previous, kWmKillFocus, hwnd, 0);
    }
    windows_.SetFocus(hwnd);
    if (hwnd != 0) {
        Send(hwnd, kWmSetFocus, previous, 0);
    }
    return previous;
}

bool User32::SetWindowPos(std::uint32_t hwnd, std::int32_t x, std::int32_t y, std::int32_t width,
                          std::int32_t height, std::uint32_t flags) {
    if (!windows_.SetPosition(hwnd, x, y, width, height, flags)) {
        return false;
    }
    if ((flags & kSwpShowWindow) != 0) {
        ShowWindow(hwnd, (flags & kSwpNoActivate) != 0 ? kSwShowNa : kSwShow);
    } else if ((flags & kSwpHideWindow) != 0) {
        ShowWindow(hwnd, kSwHide);
    }
    Send(hwnd, kWmWindowPosChanged, 0, 0);
    if ((flags & kSwpNoSize) == 0) {
        const Rect client = windows_.ClientRect(hwnd);
        Send(hwnd, kWmSize, kSizeRestored, pack(client.right, client.bottom));
    }
    return true;
}

bool User32::SetWindowText(std::uint32_t hwnd, const std::string &text) {
    if (!windows_.IsWindow(hwnd)) {
        return false;
    }
    // WM_SETTEXT carries a pointer into the game's memory; the window procedures this module
    // runs take the text from here, so the game's procedure sees the call only through
    // DefWindowProc's effect.
    return windows_.SetText(hwnd, text);
}

std::uint32_t User32::SetWindowLong(std::uint32_t hwnd, std::int32_t index, std::uint32_t value) {
    return windows_.SetLong(hwnd, index, value).value_or(0);
}

// ---- messages ----

std::uint32_t User32::Send(std::uint32_t hwnd, std::uint32_t message, std::uint32_t wParam,
                           std::uint32_t lParam) {
    const WindowRecord *record = windows_.Find(hwnd);
    if (record == nullptr) {
        return 0;
    }
    return CallProc(record->proc, hwnd, message, wParam, lParam);
}

bool User32::Post(std::uint32_t hwnd, std::uint32_t message, std::uint32_t wParam,
                  std::uint32_t lParam) {
    if (hwnd != 0 && !windows_.IsWindow(hwnd)) {
        return false;
    }
    queue_.Post(hwnd, message, wParam, lParam);
    return true;
}

Retrieved User32::Peek(std::uint32_t hwnd, std::uint32_t first, std::uint32_t last, bool remove) {
    Pump();
    const std::optional<Message> found = queue_.Peek(hwnd, first, last, remove);
    return found ? Retrieved{true, *found} : Retrieved{};
}

Message User32::Get(std::uint32_t hwnd, std::uint32_t first, std::uint32_t last) {
    if (!windows_.OnMainThread()) {
        // Not the main thread: its messages are the main thread's. Wait for the end of the run.
        while (!(stopping_ && stopping_())) {
            guest_.Idle(kWaitMs);
        }
        Message quit;
        quit.message = kWmQuit;
        quit.time = 0;
        return quit;
    }
    for (;;) {
        const Retrieved got = Peek(hwnd, first, last, true);
        if (got.found) {
            return got.message;
        }
        guest_.Idle(kWaitMs);
    }
}

std::uint32_t User32::Dispatch(const Message &message) {
    if (message.hwnd == 0 || !windows_.IsWindow(message.hwnd)) {
        return 0;
    }
    return Send(message.hwnd, message.message, message.wParam, message.lParam);
}

std::uint32_t User32::CallProc(std::uint32_t proc, std::uint32_t hwnd, std::uint32_t message,
                               std::uint32_t wParam, std::uint32_t lParam) {
    if (proc == 0) {
        return 0;
    }
    if (isModuleProc(proc)) {
        return moduleProc(proc, hwnd, message, wParam, lParam);
    }
    return guest_.CallProc(proc, hwnd, message, wParam, lParam);
}

std::uint32_t User32::moduleProc(std::uint32_t proc, std::uint32_t hwnd, std::uint32_t message,
                                 std::uint32_t wParam, std::uint32_t lParam) {
    if (proc == kDialogProc) {
        return dialogProc(hwnd, message, wParam, lParam);
    }
    if (proc == kButtonProc && (message == kBmGetCheck || message == kBmSetCheck)) {
        const WindowRecord *record = windows_.Find(hwnd);
        Dialog *dialog = record != nullptr ? dialogOf(record->parent) : nullptr;
        if (dialog == nullptr) {
            return 0;
        }
        if (message == kBmGetCheck) {
            return dialog->GetCheck(hwnd);
        }
        dialog->SetCheck(hwnd, wParam);
        return 0;
    }
    if (proc == kEditProc && message == kEmLimitText) {
        const WindowRecord *record = windows_.Find(hwnd);
        if (Dialog *dialog = record != nullptr ? dialogOf(record->parent) : nullptr) {
            dialog->SetLimit(hwnd, wParam);
        }
        return 0;
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
}

std::uint32_t User32::DefWindowProc(std::uint32_t hwnd, std::uint32_t message,
                                    std::uint32_t wParam, std::uint32_t lParam) {
    switch (message) {
    case kWmNcCreate:
        return 1;
    case kWmClose:
        DestroyWindow(hwnd);
        return 0;
    case kWmPaint:
        windows_.Validate(hwnd);
        return 0;
    case kWmEraseBkgnd:
        return 1;
    case kWmNcHitTest:
        return kHtClient;
    case kWmMouseActivate:
        return kMaActivate;
    case kWmNcActivate:
        return 1;
    case kWmActivate:
        if ((wParam & 0xFFFF) != kWaInactive && windows_.Focus() == 0) {
            SetFocus(hwnd);
        }
        return 0;
    case kWmSysCommand:
        if ((wParam & 0xFFF0) == kScClose) {
            Send(hwnd, kWmClose, 0, 0);
        }
        return 0;
    case kWmSysKeyDown:
        if (wParam == kVkF4Key && (lParam & (1u << 29)) != 0) {
            Send(hwnd, kWmSysCommand, kScClose, 0);
        }
        return 0;
    case kWmGetTextLength: {
        const WindowRecord *record = windows_.Find(hwnd);
        return record != nullptr ? static_cast<std::uint32_t>(record->text.size()) : 0;
    }
    case kWmGetText: {
        const WindowRecord *record = windows_.Find(hwnd);
        if (record == nullptr || wParam == 0 || lParam == 0) {
            return 0;
        }
        return guest_.WriteText(lParam, wParam, record->text);
    }
    default:
        return 0;
    }
}

// ---- dialogs ----

Dialog *User32::dialogOf(std::uint32_t hwnd) {
    for (const auto &dialog : dialogs_) {
        if (dialog->Handle() == hwnd) {
            return dialog.get();
        }
    }
    return nullptr;
}

std::uint32_t User32::dialogProc(std::uint32_t hwnd, std::uint32_t message, std::uint32_t wParam,
                                 std::uint32_t lParam) {
    // The game's dialog procedure first; what it leaves (FALSE) gets the default handling.
    const std::optional<std::uint32_t> procedure = windows_.GetLong(hwnd, kDwlDlgProc);
    if (procedure && *procedure != 0) {
        const std::uint32_t handled = CallProc(*procedure, hwnd, message, wParam, lParam);
        if (message == kWmInitDialog) {
            return handled;
        }
        if (handled != 0) {
            return windows_.GetLong(hwnd, kDwlMsgResult).value_or(0);
        }
    }
    switch (message) {
    case kWmClose:
        // A dialog's close box is its Cancel button.
        Post(hwnd, kWmCommand, kIdCancel, 0);
        return 0;
    case kWmCommand:
        // Without a procedure (MessageBoxA), a button ends the dialog with its id.
        if ((!procedure || *procedure == 0) && (wParam >> 16) == kBnClicked) {
            EndDialog(hwnd, static_cast<std::int32_t>(wParam & 0xFFFF));
        }
        return 0;
    default:
        return DefWindowProc(hwnd, message, wParam, lParam);
    }
}

bool User32::EndDialog(std::uint32_t hwnd, std::int32_t result) {
    Dialog *dialog = dialogOf(hwnd);
    if (dialog == nullptr) {
        return false;
    }
    dialog->End(result);
    return true;
}

std::int32_t User32::DialogBox(const DialogTemplate &dialogTemplate, std::uint32_t owner,
                               std::uint32_t instance, std::uint32_t dialogProc,
                               std::uint32_t param) {
    std::unique_ptr<Dialog> made =
        Dialog::Create(windows_, dialogTemplate, owner, instance, dialogProc);
    if (made == nullptr) {
        return -1;
    }
    Dialog &dialog = *made;
    dialogs_.push_back(std::move(made));
    const std::uint32_t hwnd = dialog.Handle();
    const std::uint32_t previousActive = windows_.Active();
    // WM_INITDIALOG's answer says whether to give the first control the focus; the dialog
    // tracks its own focus either way.
    Send(hwnd, kWmInitDialog, dialog.Focus(), param);
    std::int32_t result = 0;
    if (windows_.IsWindow(hwnd) && !dialog.Ended()) {
        ShowWindow(hwnd, kSwShow);
        result = runDialog(dialog, owner);
    } else {
        result = dialog.Result();
    }
    if (windows_.IsWindow(hwnd)) {
        DestroyWindow(hwnd);
    }
    dialogs_.erase(std::find_if(dialogs_.begin(), dialogs_.end(),
                                [&](const auto &d) { return d.get() == &dialog; }));
    if (windows_.IsWindow(previousActive)) {
        activate(previousActive);
    }
    return result;
}

// A dialog nobody can see: the first enabled push button, in template order, is pressed (never an
// id that matches no control). The game's procedure gets that button's WM_COMMAND, as a click would
// send it; when it does not end the dialog, the dialog ends with the button's id. Only the id is
// logged: a dialog's text is the game's, and can hold anything.
std::int32_t User32::answerUnseen(Dialog &dialog) {
    const std::uint32_t hwnd = dialog.Handle();
    std::optional<Dialog::Command> button = dialog.FirstPushButton();
    if (button && button->id == kIdRetry && ++retriesAnswered_ > kMaxRetryAnswers) {
        // A retry loop cannot spin: after three, the Cancel (or Abort) button is pressed.
        button = std::nullopt;
        for (const std::uint32_t control : dialog.Controls()) {
            const WindowRecord *record = windows_.Find(control);
            if (record->className == "button" && record->enabled &&
                (record->id == kIdCancel || record->id == kIdAbort)) {
                button = Dialog::Command{record->id, kBnClicked, control};
                break;
            }
        }
    }
    if (!button) {
        std::fprintf(stderr, "[user32] dialog answered: no button\n");
        if (!dialog.Ended()) {
            dialog.End(0);
        }
        return dialog.Result();
    }
    std::fprintf(stderr, "[user32] dialog answered: button %u\n", button->id);
    Send(hwnd, kWmCommand, pack(static_cast<std::int32_t>(button->id), kBnClicked),
         button->control);
    if (!dialog.Ended()) {
        dialog.End(static_cast<std::int32_t>(button->id));
    }
    return dialog.Result();
}

std::int32_t User32::runDialog(Dialog &dialog, std::uint32_t) {
    const std::uint32_t hwnd = dialog.Handle();
    if (presenter_ == nullptr || !presentDialog(dialog)) {
        return answerUnseen(dialog);
    }
    while (!dialog.Ended() && windows_.IsWindow(hwnd)) {
        const Retrieved got = Peek(kAnyWindow, 0, 0, true);
        if (!got.found) {
            guest_.Idle(kWaitMs);
            continue;
        }
        const Message &message = got.message;
        if (message.message == kWmQuit) {
            queue_.PostQuit(static_cast<std::int32_t>(message.wParam)); // for the outer loop
            break;
        }
        const bool forDialog = message.hwnd != 0 && windows_.TopLevelOf(message.hwnd) == hwnd;
        const bool input = (message.message >= kWmKeyDown && message.message <= kWmSysChar) ||
                           (message.message >= kWmMouseMove && message.message <= kWmMouseWheel);
        if (input) {
            // Input belongs to the modal dialog, wherever the queue sent it.
            routeToDialog(dialog, message);
        } else if (message.message == kWmPaint) {
            windows_.Validate(message.hwnd);
            if (forDialog) {
                presentDialog(dialog);
            }
        } else {
            Dispatch(message);
        }
    }
    return dialog.Result();
}

void User32::routeToDialog(Dialog &dialog, const Message &message) {
    std::optional<Dialog::Command> command;
    const std::uint32_t hwnd = dialog.Handle();
    switch (message.message) {
    case kWmKeyDown:
        command = dialog.OnKeyDown(message.wParam, (queue_.KeyState(kVkShift) & 0x8000) != 0);
        queue_.Translate(message);
        break;
    case kWmChar:
        command = dialog.OnChar(static_cast<char>(message.wParam));
        break;
    case kWmLButtonDown:
    case kWmLButtonUp:
    case kWmMouseMove: {
        std::int32_t ox = 0, oy = 0;
        windows_.ClientOrigin(hwnd, ox, oy);
        command = dialog.OnMouse(message.message, message.x - ox, message.y - oy);
        break;
    }
    case kWmSysKeyDown:
        DefWindowProc(hwnd, message.message, message.wParam, message.lParam);
        break;
    default:
        break;
    }
    if (command) {
        Send(hwnd, kWmCommand, pack(static_cast<std::int32_t>(command->id), command->code),
             command->control);
    }
    if (windows_.IsWindow(hwnd) && !dialog.Ended()) {
        presentDialog(dialog);
    }
}

bool User32::presentDialog(const Dialog &dialog) {
    if (presenter_ == nullptr) {
        return false;
    }
    const Rect rect = windows_.WindowRect(dialog.Handle());
    const std::int32_t width = rect.right - rect.left, height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) {
        return false;
    }
    const std::uint32_t bitmap = gdi_.CreateSurfaceBitmap(width, height);
    const std::uint32_t dc = bitmap != 0 ? gdi_.DcOnBitmap(bitmap) : 0;
    if (dc == 0) {
        gdi_.DeleteObject(bitmap);
        return false;
    }
    dialog.Draw(gdi_, dc);
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * height);
    const gdi32::Bitmap *image = gdi_.BitmapOf(bitmap);
    for (std::int32_t y = 0; y < height; ++y) {
        for (std::int32_t x = 0; x < width; ++x) {
            pixels[static_cast<std::size_t>(y) * width + x] = image->Get(x, y) & 0x00FFFFFFu;
        }
    }
    gdi_.DeleteDC(dc);
    gdi_.DeleteObject(bitmap);
    return presenter_->Show(pixels, width, height, rect.left, rect.top);
}

std::int32_t User32::MessageBox(std::uint32_t owner, const std::string &text,
                                const std::string &caption, std::uint32_t type) {
    // Nothing of the text or caption is logged (answerUnseen).
    return DialogBox(messageBoxTemplate(text, caption, type), owner, 0, 0, 0);
}

// ---- painting ----

std::uint32_t User32::BeginPaint(std::uint32_t hwnd, Rect &paint, bool &erase) {
    const WindowRecord *record = windows_.Find(hwnd);
    if (record == nullptr) {
        return 0;
    }
    paint = record->invalid ? record->invalidRect : Rect{};
    erase = record->invalid;
    windows_.Validate(hwnd);
    return GetDC(hwnd);
}

bool User32::EndPaint(std::uint32_t hwnd, std::uint32_t dc) {
    return ReleaseDC(hwnd, dc);
}

std::uint32_t User32::GetDC(std::uint32_t) {
    return gdi_.DcOnBitmap(0);
}

bool User32::ReleaseDC(std::uint32_t, std::uint32_t dc) {
    return gdi_.OwnsDc(dc) && gdi_.DeleteDC(dc);
}

// ---- input ----

std::uint16_t User32::AsyncKeyState(std::uint32_t vk) {
    bool isDown = false;
    if (vk == kVkLButton || vk == kVkRButton || vk == kVkMButton) {
        if (const auto mouse = input_.GetMouse(); mouse.Ok()) {
            const auto button = vk == kVkLButton   ? platform::MouseButton::kLeft
                                : vk == kVkRButton ? platform::MouseButton::kRight
                                                   : platform::MouseButton::kMiddle;
            isDown = mouse.Value().buttons[static_cast<std::size_t>(button)];
        }
    } else {
        platform::Scancode usages[2] = {};
        const int count = usagesOfVk(vk, usages);
        for (int i = 0; i < count && !isDown; ++i) {
            const auto down = input_.IsKeyDown(usages[i]);
            isDown = down.Ok() && down.Value();
        }
    }
    return isDown ? 0x8000 : 0;
}

// ---- the screen ----

std::int32_t User32::SystemMetric(std::uint32_t index) const {
    const gdi32::ScreenInfo screen = windows_.Screen();
    switch (index) {
    case kSmCxScreen:
    case kSmCxFullScreen:
    case kSmCxVirtualScreen:
        return screen.width;
    case kSmCyScreen:
    case kSmCyVirtualScreen:
        return screen.height;
    case kSmCyFullScreen:
        return screen.height - kCaptionHeight;
    case kSmCxVScroll:
    case kSmCyHScroll:
        return 16;
    case kSmCyCaption:
        return kCaptionHeight + 1;
    case kSmCxBorder:
    case kSmCyBorder:
        return kBorderSize;
    case kSmCxDlgFrame:
    case kSmCyDlgFrame:
        return kDlgFrameSize;
    case kSmCxFrame:
    case kSmCyFrame:
        return kFrameSize;
    case kSmCxIcon:
    case kSmCyIcon:
    case kSmCxCursor:
    case kSmCyCursor:
        return 32;
    case kSmCyMenu:
        return 19;
    case kSmMousePresent:
    case kSmCMonitors:
        return 1;
    case kSmCMouseButtons:
        return 3;
    case kSmCxDoubleClk:
    case kSmCyDoubleClk:
        return 4;
    default:
        return 0;
    }
}

std::optional<platform::DisplayMode> User32::DisplayMode(std::uint32_t index,
                                                         std::uint32_t &bits) const {
    const gdi32::ScreenInfo screen = windows_.Screen();
    if (index == kEnumCurrentSettings || index == kEnumRegistrySettings) {
        bits = static_cast<std::uint32_t>(screen.bitsPerPixel);
        return platform::DisplayMode{static_cast<std::uint32_t>(screen.width),
                                     static_cast<std::uint32_t>(screen.height),
                                     static_cast<std::uint32_t>(screen.refreshHz)};
    }
    constexpr std::size_t kCount = sizeof(kModes) / sizeof(kModes[0]);
    if (index >= kCount * 2) {
        return std::nullopt;
    }
    bits = index % 2 == 0 ? 16 : 32;
    return platform::DisplayMode{kModes[index / 2].width, kModes[index / 2].height, 60};
}

// ---- the presenter on Gpu ----

std::optional<std::vector<std::uint8_t>> pixelsAs(const std::vector<std::uint32_t> &pixels,
                                                  platform::PixelFormat format) {
    std::vector<std::uint8_t> out;
    switch (format) {
    case platform::PixelFormat::kXrgb8888:
    case platform::PixelFormat::kArgb8888:
        out.reserve(pixels.size() * 4);
        for (const std::uint32_t p : pixels) {
            const std::uint32_t v = p | 0xFF000000u;
            out.insert(out.end(), {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8),
                                   static_cast<std::uint8_t>(v >> 16),
                                   static_cast<std::uint8_t>(v >> 24)});
        }
        return out;
    case platform::PixelFormat::kRgb565:
    case platform::PixelFormat::kArgb1555:
        out.reserve(pixels.size() * 2);
        for (const std::uint32_t p : pixels) {
            const std::uint32_t r = p >> 16 & 0xFF, g = p >> 8 & 0xFF, b = p & 0xFF;
            const std::uint16_t v =
                format == platform::PixelFormat::kRgb565
                    ? static_cast<std::uint16_t>((r >> 3) << 11 | (g >> 2) << 5 | b >> 3)
                    : static_cast<std::uint16_t>(0x8000 | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3);
            out.push_back(static_cast<std::uint8_t>(v));
            out.push_back(static_cast<std::uint8_t>(v >> 8));
        }
        return out;
    default:
        return std::nullopt;
    }
}

namespace {

class GpuPresenter final : public Presenter {
public:
    GpuPresenter(platform::Gpu &gpu, std::function<std::optional<DisplayTarget>()> display)
        : gpu_(gpu), display_(std::move(display)) {}

    bool Show(const std::vector<std::uint32_t> &pixels, std::int32_t width, std::int32_t height,
              std::int32_t x, std::int32_t y) override {
        const std::optional<DisplayTarget> target = display_ ? display_() : std::nullopt;
        if (!target) {
            return false;
        }
        // The part of the dialog on the display.
        const std::int32_t left = std::max(x, 0), top = std::max(y, 0);
        const std::int32_t right = std::min(x + width, static_cast<std::int32_t>(target->width));
        const std::int32_t bottom = std::min(y + height, static_cast<std::int32_t>(target->height));
        if (right <= left || bottom <= top) {
            return false;
        }
        std::vector<std::uint32_t> part;
        part.reserve(static_cast<std::size_t>(right - left) * (bottom - top));
        for (std::int32_t row = top; row < bottom; ++row) {
            const auto *from = &pixels[static_cast<std::size_t>(row - y) * width + (left - x)];
            part.insert(part.end(), from, from + (right - left));
        }
        const auto bytes = pixelsAs(part, target->format);
        if (!bytes) {
            return false;
        }
        const platform::Rect region{left, top,
                                    static_cast<std::uint32_t>(right - left),
                                    static_cast<std::uint32_t>(bottom - top)};
        const std::uint32_t pitch =
            static_cast<std::uint32_t>(right - left) * platform::pixelBytes(target->format);
        if (!gpu_.WriteTexture(target->texture, 0, region, bytes->data(), pitch).Ok()) {
            return false;
        }
        static_cast<void>(gpu_.Present().Ok());
        return true;
    }

private:
    platform::Gpu &gpu_;
    std::function<std::optional<DisplayTarget>()> display_;
};

} // namespace

std::unique_ptr<Presenter> makeGpuPresenter(platform::Gpu &gpu,
                                            std::function<std::optional<DisplayTarget>()> display) {
    return std::make_unique<GpuPresenter>(gpu, std::move(display));
}

} // namespace sfcop::hle::user32
