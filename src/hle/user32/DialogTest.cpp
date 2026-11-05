#include "hle/user32/Dialog.h"
#include "hle/user32/User32TestRig.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace sfcop::hle::user32 {

using platform::WindowEventType;
using test::kGameDialogProc;
using test::Rig;

namespace {

// A template's bytes, written field by field as the Win32 headers lay them out.
struct Bytes {
    std::vector<std::uint8_t> data;
    void u16(std::uint16_t v) {
        data.push_back(static_cast<std::uint8_t>(v));
        data.push_back(static_cast<std::uint8_t>(v >> 8));
    }
    void u32(std::uint32_t v) {
        u16(static_cast<std::uint16_t>(v));
        u16(static_cast<std::uint16_t>(v >> 16));
    }
    void text(const std::string &s) {
        for (const char c : s) {
            u16(static_cast<std::uint8_t>(c));
        }
        u16(0);
    }
    void align4() {
        while (data.size() % 4 != 0) {
            data.push_back(0);
        }
    }
};

// The game's "Choose a Display" dialog with two displays: DS_SETFONT, 8-point Arial, a button
// per display with ids from 10000.
std::vector<std::uint8_t> chooseDisplay() {
    Bytes b;
    b.u32(0x90C80BC4u); // style
    b.u32(0);           // exStyle
    b.u16(2);           // items
    b.u16(0), b.u16(0), b.u16(160), b.u16(60);
    b.u16(0);           // no menu
    b.u16(0);           // the dialog class
    b.text("Choose a Display");
    b.u16(8);
    b.text("Arial");
    const char *names[] = {"Primary", "Accelerator"};
    for (int i = 0; i < 2; ++i) {
        b.align4();
        b.u32(0x50010000u); // WS_CHILD | WS_VISIBLE | WS_TABSTOP, BS_PUSHBUTTON
        b.u32(0);
        b.u16(static_cast<std::uint16_t>(10 + 80 * i)), b.u16(10), b.u16(60), b.u16(14);
        b.u16(static_cast<std::uint16_t>(10000 + i));
        b.u16(0xFFFF), b.u16(0x0080); // button
        b.text(names[i]);
        b.u16(0); // no creation data
    }
    return b.data;
}

class FakePresenter final : public Presenter {
public:
    int shown = 0;
    std::vector<std::uint32_t> pixels;
    std::int32_t width = 0, height = 0, x = 0, y = 0;
    bool shows = true; // false: there is no display to draw on
    bool Show(const std::vector<std::uint32_t> &p, std::int32_t w, std::int32_t h, std::int32_t px,
              std::int32_t py) override {
        ++shown;
        pixels = p, width = w, height = h, x = px, y = py;
        return shows;
    }
    std::uint32_t At(std::int32_t px, std::int32_t py) const {
        return pixels[static_cast<std::size_t>(py) * width + px];
    }
};

} // namespace

TEST_CASE("A DLGTEMPLATE with DS_SETFONT and two buttons parses field by field") {
    const std::vector<std::uint8_t> bytes = chooseDisplay();
    const auto dialog = parseDialogTemplate(bytes.data(), bytes.size());
    REQUIRE(dialog);
    CHECK(dialog->style == 0x90C80BC4u);
    CHECK(dialog->cx == 160);
    CHECK(dialog->cy == 60);
    CHECK(dialog->title == "Choose a Display");
    CHECK(dialog->pointSize == 8);
    CHECK(dialog->font == "Arial");
    REQUIRE(dialog->items.size() == 2);
    CHECK(dialog->items[0].className == "button");
    CHECK(dialog->items[0].text == "Primary");
    CHECK(dialog->items[0].id == 10000);
    CHECK(dialog->items[1].x == 90);
    CHECK(dialog->items[1].text == "Accelerator");
}

TEST_CASE("A DLGTEMPLATEEX parses, and a template cut short does not") {
    Bytes b;
    b.u16(1), b.u16(0xFFFF); // dlgVer, signature
    b.u32(0);                // helpID
    b.u32(0);                // exStyle
    b.u32(0x80C80040u);      // style, DS_SETFONT
    b.u16(1);
    b.u16(0), b.u16(0), b.u16(100), b.u16(50);
    b.u16(0), b.u16(0);
    b.text("Ex");
    b.u16(9), b.u16(400), b.data.push_back(0), b.data.push_back(1);
    b.text("Tahoma");
    b.align4();
    b.u32(0);           // helpID
    b.u32(0);           // exStyle
    b.u32(0x50800080u); // WS_CHILD | WS_VISIBLE | WS_BORDER, ES_AUTOHSCROLL
    b.u16(5), b.u16(5), b.u16(80), b.u16(12);
    b.u32(77);
    b.u16(0xFFFF), b.u16(0x0081); // edit
    b.u16(0xFFFF), b.u16(3);      // an ordinal title
    b.u16(0);
    auto dialog = parseDialogTemplate(b.data.data(), b.data.size());
    REQUIRE(dialog);
    CHECK(dialog->font == "Tahoma");
    CHECK(dialog->pointSize == 9);
    REQUIRE(dialog->items.size() == 1);
    CHECK(dialog->items[0].className == "edit");
    CHECK(dialog->items[0].id == 77);
    CHECK(dialog->items[0].text == "#3");

    CHECK_FALSE(parseDialogTemplate(b.data.data(), b.data.size() - 6));
    CHECK_FALSE(parseDialogTemplate(nullptr, 0));
}

TEST_CASE("The dialog is centred for DS_CENTER, with its controls in dialog units") {
    Rig rig;
    const std::vector<std::uint8_t> bytes = chooseDisplay();
    const auto parsed = parseDialogTemplate(bytes.data(), bytes.size());
    auto dialog = Dialog::Create(rig.user.Windows(), *parsed, 0, 0x00400000, kGameDialogProc);
    REQUIRE(dialog);
    WindowManager &windows = rig.user.Windows();
    // 160x60 units are 240x97 pixels; a caption and a dialog frame make it 246x121.
    const Rect r = windows.WindowRect(dialog->Handle());
    CHECK(r.left == 197);
    CHECK(r.top == 179);
    CHECK(r.right == 443);
    CHECK(r.bottom == 300);
    CHECK(windows.GetLong(dialog->Handle(), kDwlDlgProc) == kGameDialogProc);
    REQUIRE(dialog->Controls().size() == 2);
    const Rect button = windows.Find(dialog->Controls()[1])->rect;
    CHECK(button.left == 135);
    CHECK(button.top == 16);
    CHECK(button.right == 225);
    CHECK(button.bottom == 38);
    CHECK(dialog->Focus() == dialog->Controls()[0]);
    CHECK(dialog->DefaultId() == kIdOk); // no BS_DEFPUSHBUTTON
}

TEST_CASE("Tab moves the focus; Enter presses the focused button; Escape asks IDCANCEL") {
    Rig rig;
    const std::vector<std::uint8_t> bytes = chooseDisplay();
    auto dialog = Dialog::Create(rig.user.Windows(), *parseDialogTemplate(bytes.data(), bytes.size()),
                                 0, 0, kGameDialogProc);
    REQUIRE(dialog);
    const std::uint32_t first = dialog->Controls()[0], second = dialog->Controls()[1];
    CHECK_FALSE(dialog->OnKeyDown(kVkTab, false));
    CHECK(dialog->Focus() == second);
    CHECK_FALSE(dialog->OnKeyDown(kVkTab, false));
    CHECK(dialog->Focus() == first);
    CHECK_FALSE(dialog->OnKeyDown(kVkTab, true));
    CHECK(dialog->Focus() == second);
    auto command = dialog->OnKeyDown(kVkReturn, false);
    REQUIRE(command);
    CHECK(command->id == 10001);
    CHECK(command->code == kBnClicked);
    CHECK(command->control == second);
    command = dialog->OnKeyDown(kVkEscape, false);
    REQUIRE(command);
    CHECK(command->id == kIdCancel);
    CHECK(command->control == 0);
}

TEST_CASE("A button is pressed when the mouse goes down and up over it") {
    Rig rig;
    const std::vector<std::uint8_t> bytes = chooseDisplay();
    auto dialog = Dialog::Create(rig.user.Windows(), *parseDialogTemplate(bytes.data(), bytes.size()),
                                 0, 0, kGameDialogProc);
    CHECK_FALSE(dialog->OnMouse(kWmLButtonDown, 20, 20));
    auto command = dialog->OnMouse(kWmLButtonUp, 30, 30);
    REQUIRE(command);
    CHECK(command->id == 10000);
    // Down on one, up on the other: nothing.
    CHECK_FALSE(dialog->OnMouse(kWmLButtonDown, 20, 20));
    CHECK_FALSE(dialog->OnMouse(kWmLButtonUp, 140, 20));
    // Up with nothing pressed: nothing.
    CHECK_FALSE(dialog->OnMouse(kWmLButtonUp, 140, 20));
}

TEST_CASE("An edit takes characters up to its limit, digits only for ES_NUMBER") {
    Rig rig;
    DialogTemplate t;
    t.style = kWsPopup;
    t.cx = 100, t.cy = 40;
    DialogItemTemplate edit;
    edit.style = kWsChild | kWsVisible | kWsTabStop | kEsNumber;
    edit.cx = 80, edit.cy = 12;
    edit.id = 5;
    edit.className = "edit";
    t.items.push_back(edit);
    auto dialog = Dialog::Create(rig.user.Windows(), t, 0, 0, 0);
    REQUIRE(dialog);
    const std::uint32_t control = dialog->Controls()[0];
    dialog->SetLimit(control, 3);
    CHECK_FALSE(dialog->OnChar('x'));
    for (const char c : std::string("1234")) {
        dialog->OnChar(c);
    }
    CHECK(rig.user.Windows().Find(control)->text == "123");
    const auto command = dialog->OnKeyDown(kVkBack, false);
    REQUIRE(command);
    CHECK(command->code == kEnChange);
    CHECK(rig.user.Windows().Find(control)->text == "12");
}

TEST_CASE("Without a screen, DialogBoxIndirectParamA presses the first push button at once") {
    Rig rig;
    std::uint32_t dialogHwnd = 0;
    rig.guest.procs[kGameDialogProc] = [&](const test::Call &call) -> std::uint32_t {
        if (call.message == kWmInitDialog) {
            dialogHwnd = call.hwnd;
            CHECK(call.lParam == 0x1234u);
            return 1;
        }
        if (call.message == kWmCommand) {
            rig.user.EndDialog(call.hwnd, static_cast<std::int32_t>(call.wParam & 0xFFFF));
            return 1;
        }
        return 0;
    };
    const std::vector<std::uint8_t> bytes = chooseDisplay();
    const std::int32_t result = rig.user.DialogBox(
        *parseDialogTemplate(bytes.data(), bytes.size()), 0, 0, kGameDialogProc, 0x1234);
    CHECK(result == 10000); // the first button's id; the template names no default, and no IDOK
    CHECK_FALSE(rig.user.Windows().IsWindow(dialogHwnd));
}

namespace {

// A dialog whose buttons have ids above 1 and none the default: [static 99] [disabled push 20]
// [check box 21] [push 22] [push 23].
DialogTemplate plainButtons() {
    DialogTemplate t;
    t.style = kWsPopup | kWsCaption;
    t.cx = 120, t.cy = 60;
    auto item = [&](const char *cls, std::uint32_t style, std::uint32_t id) {
        DialogItemTemplate i;
        i.className = cls;
        i.style = kWsChild | kWsVisible | style;
        i.id = id;
        i.cx = 40, i.cy = 14;
        t.items.push_back(i);
    };
    item("static", 0, 99);
    item("button", kWsDisabled, 20);    // BS_PUSHBUTTON, disabled
    item("button", 0x02, 21);           // BS_CHECKBOX
    item("button", 0, 22);              // BS_PUSHBUTTON
    item("button", 0, 23);              // BS_PUSHBUTTON
    return t;
}

} // namespace

TEST_CASE("A dialog nobody can see answers with the first enabled push button, in template order") {
    Rig rig;
    std::vector<std::uint32_t> commands;
    rig.guest.procs[kGameDialogProc] = [&](const test::Call &call) -> std::uint32_t {
        if (call.message == kWmCommand) {
            commands.push_back(call.wParam);
        }
        return 0; // the game's procedure does not end it: the dialog ends with the id
    };
    CHECK(rig.user.DialogBox(plainButtons(), 0, 0, kGameDialogProc, 0) == 22);
    REQUIRE(commands.size() == 1);
    CHECK(commands[0] == (22u | (kBnClicked << 16))); // the WM_COMMAND a click sends
}

TEST_CASE("A presenter that shows nothing leaves the dialog to the same rule") {
    Rig rig;
    auto presenter = std::make_unique<FakePresenter>();
    presenter->shows = false;
    FakePresenter &seen = *presenter;
    rig.user.SetPresenter(std::move(presenter));
    CHECK(rig.user.DialogBox(plainButtons(), 0, 0, 0, 0) == 22);
    CHECK(seen.shown == 1); // asked once, then answered unseen
}

TEST_CASE("A dialog with no push button ends with 0, never an id that matches no control") {
    Rig rig;
    DialogTemplate t;
    t.style = kWsPopup;
    t.cx = 60, t.cy = 30;
    DialogItemTemplate label;
    label.className = "static";
    label.style = kWsChild | kWsVisible;
    label.id = 7;
    t.items.push_back(label);
    CHECK(rig.user.DialogBox(t, 0, 0, 0, 0) == 0);
}

TEST_CASE("On a screen, the dialog is drawn and a click on a button ends it") {
    Rig rig;
    auto presenter = std::make_unique<FakePresenter>();
    FakePresenter &seen = *presenter;
    rig.user.SetPresenter(std::move(presenter));
    const std::uint32_t game = rig.GameWindow();
    rig.Drain();
    rig.guest.procs[kGameDialogProc] = [&](const test::Call &call) -> std::uint32_t {
        if (call.message == kWmCommand && (call.wParam & 0xFFFF) >= 10000) {
            rig.user.EndDialog(call.hwnd, static_cast<std::int32_t>(call.wParam & 0xFFFF));
            return 1;
        }
        return call.message == kWmInitDialog ? 1 : 0;
    };
    rig.guest.onIdle = [&] {
        if (rig.guest.idled == 1) {
            // The client area starts at (200, 200): the first button is at (215, 216).
            rig.Mouse(WindowEventType::kMouseButtonDown, 220, 220);
            rig.Mouse(WindowEventType::kMouseButtonUp, 221, 221);
        }
    };
    const std::vector<std::uint8_t> bytes = chooseDisplay();
    const std::int32_t result = rig.user.DialogBox(
        *parseDialogTemplate(bytes.data(), bytes.size()), game, 0, kGameDialogProc, 0);
    CHECK(result == 10000);
    CHECK(seen.shown >= 2);
    CHECK(seen.x == 197);
    CHECK(seen.y == 179);
    CHECK(seen.width == 246);
    CHECK(seen.height == 121);
    CHECK(seen.At(10, 10) == 0x000080u);  // the caption, navy
    CHECK(seen.At(5, 60) == 0xC0C0C0u);   // the face, grey
    CHECK(seen.At(0, 0) == 0xC0C0C0u);    // the outer frame's light edge
    CHECK(seen.At(245, 120) == 0x000000u); // and its dark edge
    CHECK(rig.user.Windows().Active() == game); // the owner is active again
}

TEST_CASE("MessageBoxA headless: Retry is pressed three times, then Cancel") {
    Rig rig;
    for (int i = 0; i < 3; ++i) {
        CHECK(rig.user.MessageBox(0, "Cannot read the disc", "Error", kMbRetryCancel) == kIdRetry);
    }
    CHECK(rig.user.MessageBox(0, "Cannot read the disc", "Error", kMbRetryCancel) == kIdCancel);
    CHECK(rig.user.MessageBox(0, "Cannot read the disc", "Error", kMbRetryCancel) == kIdCancel);
}

TEST_CASE("MessageBoxA answers its first button headless, and Escape on a screen") {
    Rig rig;
    CHECK(rig.user.MessageBox(0, "Disk full", "Error", kMbOkCancel) == kIdOk);
    CHECK(rig.user.MessageBox(0, "Quit?", "", kMbYesNo) == kIdYes);

    rig.user.SetPresenter(std::make_unique<FakePresenter>());
    rig.guest.onIdle = [&] {
        if (rig.guest.idled == 1) {
            rig.Key(0x29, true); // Escape
        }
    };
    CHECK(rig.user.MessageBox(0, "Disk full", "Error", kMbOkCancel) == kIdCancel);
}

TEST_CASE("A template in the game's memory is read as far as its own layout goes") {
    // chooseDisplay() followed by garbage the template does not mention: the read stops at the end
    // of the second item.
    std::vector<std::uint8_t> bytes = chooseDisplay();
    const std::size_t length = bytes.size();
    bytes.insert(bytes.end(), 64, 0xEE);
    const auto dialog = parseDialogTemplate(bytes.data());
    REQUIRE(dialog);
    CHECK(dialog->items.size() == 2);
    CHECK(dialog->items[1].text == "Accelerator");
    CHECK(length < bytes.size());
}

TEST_CASE("A string with no end in a template fails the read instead of running on") {
    Bytes b;
    b.u32(0x80C80000u), b.u32(0);
    b.u16(0);
    b.u16(0), b.u16(0), b.u16(10), b.u16(10);
    b.u16(0), b.u16(0);
    for (int i = 0; i < 400; ++i) {
        b.u16('A'); // a title longer than any field may be, never terminated
    }
    CHECK_FALSE(parseDialogTemplate(b.data.data()));
}

TEST_CASE("The message box template has the text and the buttons the type asks for") {
    const DialogTemplate t = messageBoxTemplate("line one\nline two", "Title", kMbYesNoCancel);
    CHECK(t.title == "Title");
    REQUIRE(t.items.size() == 4);
    CHECK(t.items[0].className == "static");
    CHECK(t.items[0].cy == 16);
    CHECK(t.items[1].id == kIdYes);
    CHECK((t.items[1].style & kBsTypeMask) == kBsDefPushButton);
    CHECK(t.items[2].id == kIdNo);
    CHECK(t.items[3].id == kIdCancel);
    CHECK((t.items[3].style & kBsTypeMask) == 0);
}

} // namespace sfcop::hle::user32
