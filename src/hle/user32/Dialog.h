// Dialog: a modal dialog as USER32 runs one (DESIGN §4.7, §5.6). A template in memory
// (DLGTEMPLATE or DLGTEMPLATEEX) becomes a dialog window with its controls in WindowManager;
// keys and the mouse move the focus, type into edits and press buttons; and the dialog is drawn
// with this project's GDI onto a bitmap that User32's Presenter puts on the screen.
//
// Buttons, edits and statics are the controls run here; any other class is drawn as an empty
// frame. The dialog procedure is the game's: what a key or click does comes back as a Command,
// the WM_COMMAND User32 sends it. Nothing here calls the game.
//
// The game's one dialog is "Choose a Display" (DialogBoxIndirectParamA, a template it builds in
// memory with one button per display). MessageBoxA is a dialog of the same kind
// (messageBoxTemplate).
#pragma once

#include "hle/gdi32/Gdi.h"
#include "hle/user32/WindowManager.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sfcop::hle::user32 {

struct DialogItemTemplate {
    std::uint32_t style = 0;
    std::uint32_t exStyle = 0;
    std::int16_t x = 0, y = 0, cx = 0, cy = 0; // dialog units
    std::uint32_t id = 0;
    std::string className; // "button", "edit", "static", ... for the predefined atoms
    std::string text;      // "#<n>" for an ordinal
};

struct DialogTemplate {
    std::uint32_t style = 0;
    std::uint32_t exStyle = 0;
    std::int16_t x = 0, y = 0, cx = 0, cy = 0;
    std::string menu;
    std::string className; // empty: the dialog class
    std::string title;
    std::uint16_t pointSize = 0; // DS_SETFONT only
    std::string font;
    std::vector<DialogItemTemplate> items;
};

// Reads a template laid out as the Win32 headers give it, little-endian, strings in UTF-16 (made
// ASCII here, '?' for anything else). Nothing if the bytes end before the template does.
std::optional<DialogTemplate> parseDialogTemplate(const std::uint8_t *data, std::size_t size);
// The same for a template in the game's memory, whose length is not known: the read ends where the
// template's own item count and layout end, and no string may be longer than 256 characters.
std::optional<DialogTemplate> parseDialogTemplate(const std::uint8_t *data);

// MessageBoxA's dialog: the text, and the buttons the type asks for (MB_OK, MB_OKCANCEL,
// MB_YESNO, MB_YESNOCANCEL, MB_RETRYCANCEL), the first the default.
DialogTemplate messageBoxTemplate(const std::string &text, const std::string &caption,
                                  std::uint32_t type);

// Dialog units to pixels, for the font this module draws (base units 6 by 13).
constexpr std::int32_t kBaseUnitX = 6;
constexpr std::int32_t kBaseUnitY = 13;
constexpr std::int32_t dialogX(std::int32_t units) {
    return units * kBaseUnitX / 4;
}
constexpr std::int32_t dialogY(std::int32_t units) {
    return units * kBaseUnitY / 8;
}

// DS_CENTER.
constexpr std::uint32_t kDsCenter = 0x0800;

class Dialog {
public:
    // What a key or click asks of the dialog procedure: WM_COMMAND with the control's id and the
    // notification code in wParam, the control's window in lParam.
    struct Command {
        std::uint32_t id = 0;
        std::uint32_t code = 0;
        std::uint32_t control = 0;
    };

    // The dialog window and its controls, hidden; nothing if a window cannot be made. The
    // dialog is centred on the screen for DS_CENTER or when it has no owner, else placed in the
    // owner's client area. `dialogProc` is stored in DWL_DLGPROC.
    static std::unique_ptr<Dialog> Create(WindowManager &windows, const DialogTemplate &dialog,
                                          std::uint32_t owner, std::uint32_t instance,
                                          std::uint32_t dialogProc);

    std::uint32_t Handle() const { return handle_; }
    const std::vector<std::uint32_t> &Controls() const { return controls_; }

    // ---- input ----
    // A key going down: Tab and the arrows move the focus, Enter presses the default button,
    // Escape asks for IDCANCEL, Space presses the focused button, Backspace edits.
    std::optional<Command> OnKeyDown(std::uint32_t vk, bool shift);
    // A character typed into the focused edit.
    std::optional<Command> OnChar(char c);
    // A mouse message (WM_LBUTTONDOWN, WM_LBUTTONUP, WM_MOUSEMOVE) at a point in the dialog's
    // client area: a button is pressed between the button going down and up over it.
    std::optional<Command> OnMouse(std::uint32_t message, std::int32_t x, std::int32_t y);

    // ---- state ----
    std::uint32_t Focus() const { return focus_; }
    void SetFocus(std::uint32_t control);
    // The id of the default push button, else IDOK.
    std::uint32_t DefaultId() const;
    // The first enabled push button in template order (what a dialog nobody can see answers with):
    // its id and window; nothing when the dialog has none.
    std::optional<Command> FirstPushButton() const;
    std::uint32_t GetCheck(std::uint32_t control) const;
    void SetCheck(std::uint32_t control, std::uint32_t state);
    void SetLimit(std::uint32_t control, std::size_t limit);

    void End(std::int32_t result) {
        ended_ = true;
        result_ = result;
    }
    bool Ended() const { return ended_; }
    std::int32_t Result() const { return result_; }

    // ---- drawing ----
    // The whole dialog window (frame, caption, controls) onto `dc`, whose (0, 0) is the window's
    // top-left corner.
    void Draw(gdi32::Gdi &gdi, std::uint32_t dc) const;

private:
    Dialog(WindowManager &windows, std::uint32_t handle) : windows_(windows), handle_(handle) {}

    enum class Kind : std::uint8_t { kButton, kCheckBox, kGroupBox, kEdit, kStatic, kOther };
    Kind kindOf(std::uint32_t control) const;
    bool focusable(std::uint32_t control) const;
    void moveFocus(int step);
    std::uint32_t controlAt(std::int32_t x, std::int32_t y) const;
    std::optional<Command> press(std::uint32_t control);
    void drawControl(gdi32::Gdi &gdi, std::uint32_t dc, std::uint32_t control, std::int32_t ox,
                     std::int32_t oy) const;

    WindowManager &windows_;
    std::uint32_t handle_;
    std::vector<std::uint32_t> controls_;
    std::uint32_t focus_ = 0;
    std::uint32_t pressed_ = 0; // the button the mouse went down on
    bool pressedInside_ = false;
    std::map<std::uint32_t, std::uint32_t> checks_;
    std::map<std::uint32_t, std::size_t> limits_;
    bool ended_ = false;
    std::int32_t result_ = 0;
};

} // namespace sfcop::hle::user32
