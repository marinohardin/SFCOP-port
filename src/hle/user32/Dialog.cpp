#include "hle/user32/Dialog.h"

#include "hle/gdi32/Gdi32Defs.h"
#include "hle/user32/User32Defs.h"

#include <algorithm>
#include <limits>

namespace sfcop::hle::user32 {

namespace {

using gdi32::Colorref;
using gdi32::rgb;

constexpr std::uint32_t kDsShellFont = 0x48; // DS_SETFONT | DS_FIXEDSYS
constexpr std::uint32_t kBsPushButton = 0x00;
constexpr std::uint32_t kBsAutoRadioButton = 0x09;
constexpr std::uint32_t kVkUp = 0x26;
constexpr std::uint32_t kVkDown = 0x28;

// The classic scheme's colours (COLOR_BTNFACE and the rest).
constexpr Colorref kFace = rgb(192, 192, 192);
constexpr Colorref kHighlight = rgb(255, 255, 255);
constexpr Colorref kShadow = rgb(128, 128, 128);
constexpr Colorref kDark = rgb(0, 0, 0);
constexpr Colorref kCaption = rgb(0, 0, 128);
constexpr Colorref kCaptionText = rgb(255, 255, 255);
constexpr Colorref kWindowBack = rgb(255, 255, 255);
constexpr Colorref kText = rgb(0, 0, 0);
constexpr Colorref kGreyText = rgb(128, 128, 128);

// The longest string a template field may hold (a caption, a font face, a control's text).
constexpr std::size_t kMaxStringUnits = 256;

class Reader {
public:
    Reader(const std::uint8_t *data, std::size_t size) : data_(data), size_(size) {}

    bool ok() const { return ok_; }
    std::uint16_t u16() {
        if (pos_ + 2 > size_) {
            ok_ = false;
            return 0;
        }
        const auto value = static_cast<std::uint16_t>(data_[pos_] | data_[pos_ + 1] << 8);
        pos_ += 2;
        return value;
    }
    std::uint32_t u32() {
        const std::uint32_t low = u16();
        return low | static_cast<std::uint32_t>(u16()) << 16;
    }
    std::uint8_t u8() {
        if (pos_ + 1 > size_) {
            ok_ = false;
            return 0;
        }
        return data_[pos_++];
    }
    std::int16_t i16() { return static_cast<std::int16_t>(u16()); }
    void align4() { pos_ = (pos_ + 3) & ~std::size_t{3}; }
    void skip(std::size_t bytes) {
        pos_ += bytes;
        ok_ = ok_ && pos_ <= size_;
    }

    // A zero-terminated UTF-16 string of at most kMaxStringUnits characters; a longer one (or one
    // with no end in the bytes) fails the read.
    std::string text() {
        std::string out;
        std::size_t units = 0;
        for (std::uint16_t c = u16(); ok_ && c != 0; c = u16()) {
            if (++units > kMaxStringUnits) {
                ok_ = false;
                break;
            }
            out += c < 0x80 ? static_cast<char>(c) : '?';
        }
        return out;
    }
    // sz_Or_Ord: none (0), an ordinal (0xFFFF then the number, given as "#n"), or a string.
    std::string nameOrOrdinal() {
        const std::uint16_t first = u16();
        if (first == 0) {
            return {};
        }
        if (first == 0xFFFF) {
            return "#" + std::to_string(u16());
        }
        pos_ -= 2;
        return text();
    }

private:
    const std::uint8_t *data_;
    std::size_t size_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

// The predefined control classes a template names by atom.
std::string predefinedClass(const std::string &name) {
    static const char *const kNames[] = {"button", "edit", "static", "listbox", "scrollbar",
                                         "combobox"};
    if (name.size() > 1 && name[0] == '#') {
        const unsigned long atom = std::strtoul(name.c_str() + 1, nullptr, 10);
        if (atom >= 0x80 && atom <= 0x85) {
            return kNames[atom - 0x80];
        }
    }
    return name;
}

void frame3d(gdi32::Gdi &gdi, std::uint32_t dc, const Rect &r, Colorref topLeft,
             Colorref bottomRight) {
    gdi.FillSolid(dc, Rect{r.left, r.top, r.right, r.top + 1}, topLeft);
    gdi.FillSolid(dc, Rect{r.left, r.top, r.left + 1, r.bottom}, topLeft);
    gdi.FillSolid(dc, Rect{r.left, r.bottom - 1, r.right, r.bottom}, bottomRight);
    gdi.FillSolid(dc, Rect{r.right - 1, r.top, r.right, r.bottom}, bottomRight);
}

Rect inset(const Rect &r, std::int32_t by) {
    return Rect{r.left + by, r.top + by, r.right - by, r.bottom - by};
}

void text(gdi32::Gdi &gdi, std::uint32_t dc, std::int32_t x, std::int32_t y, const std::string &s,
          Colorref color) {
    gdi.SetTextColor(dc, color);
    gdi.TextOut(dc, x, y, s);
}

// The text as drawn: '&' marks the next character's mnemonic and is not drawn ("&&" draws '&').
std::string shown(const std::string &s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&' && i + 1 < s.size()) {
            ++i;
        }
        out += s[i];
    }
    return out;
}

} // namespace

// ---- templates ----

std::optional<DialogTemplate> parseDialogTemplate(const std::uint8_t *data, std::size_t size) {
    if (data == nullptr) {
        return std::nullopt;
    }
    Reader in(data, size);
    DialogTemplate dialog;
    const bool extended = size >= 4 && data[0] == 1 && data[1] == 0 && data[2] == 0xFF &&
                          data[3] == 0xFF;
    std::uint16_t count = 0;
    if (extended) {
        in.skip(4); // dlgVer, signature
        in.u32();   // helpID
        dialog.exStyle = in.u32();
        dialog.style = in.u32();
    } else {
        dialog.style = in.u32();
        dialog.exStyle = in.u32();
    }
    count = in.u16();
    dialog.x = in.i16(), dialog.y = in.i16(), dialog.cx = in.i16(), dialog.cy = in.i16();
    dialog.menu = in.nameOrOrdinal();
    dialog.className = in.nameOrOrdinal();
    dialog.title = in.text();
    if ((dialog.style & kDsSetFont) != 0 || (extended && (dialog.style & kDsShellFont) != 0)) {
        dialog.pointSize = in.u16();
        if (extended) {
            in.u16(); // weight
            in.u8();  // italic
            in.u8();  // charset
        }
        dialog.font = in.text();
    }
    for (std::uint16_t i = 0; i < count && in.ok(); ++i) {
        in.align4();
        DialogItemTemplate item;
        if (extended) {
            in.u32(); // helpID
            item.exStyle = in.u32();
            item.style = in.u32();
        } else {
            item.style = in.u32();
            item.exStyle = in.u32();
        }
        item.x = in.i16(), item.y = in.i16(), item.cx = in.i16(), item.cy = in.i16();
        item.id = extended ? in.u32() : in.u16();
        item.className = predefinedClass(in.nameOrOrdinal());
        item.text = in.nameOrOrdinal();
        in.skip(in.u16()); // creation data
        dialog.items.push_back(std::move(item));
    }
    if (!in.ok()) {
        return std::nullopt;
    }
    return dialog;
}

std::optional<DialogTemplate> parseDialogTemplate(const std::uint8_t *data) {
    // The template's own layout ends the read: its item count, its strings (each bounded) and each
    // item's creation data (a 16-bit length).
    return parseDialogTemplate(data, std::numeric_limits<std::size_t>::max() / 2);
}

DialogTemplate messageBoxTemplate(const std::string &message, const std::string &caption,
                                  std::uint32_t type) {
    std::vector<std::pair<std::uint32_t, const char *>> buttons;
    switch (type & kMbTypeMask) {
    case kMbOkCancel:
        buttons = {{kIdOk, "OK"}, {kIdCancel, "Cancel"}};
        break;
    case kMbYesNoCancel:
        buttons = {{kIdYes, "&Yes"}, {kIdNo, "&No"}, {kIdCancel, "Cancel"}};
        break;
    case kMbYesNo:
        buttons = {{kIdYes, "&Yes"}, {kIdNo, "&No"}};
        break;
    case kMbRetryCancel:
        buttons = {{kIdRetry, "&Retry"}, {kIdCancel, "Cancel"}};
        break;
    default:
        buttons = {{kIdOk, "OK"}};
        break;
    }
    // The text's lines decide the size; 6 pixels a character is 4 dialog units.
    std::int16_t lines = 1, longest = 0, current = 0;
    for (const char c : message) {
        if (c == '\n') {
            ++lines, current = 0;
        } else {
            longest = std::max<std::int16_t>(longest, ++current);
        }
    }
    const auto buttonsWidth = static_cast<std::int16_t>(buttons.size() * 54 - 4);
    const auto width = static_cast<std::int16_t>(std::max<int>({longest * 4 + 16, buttonsWidth + 16, 120}));
    const auto textHeight = static_cast<std::int16_t>(lines * 8);
    DialogTemplate dialog;
    dialog.style = kWsPopup | kWsCaption | kDsCenter | kDsSetFont;
    dialog.cx = width;
    dialog.cy = static_cast<std::int16_t>(textHeight + 38);
    dialog.title = caption.empty() ? "Error" : caption;
    dialog.pointSize = 8;
    dialog.font = "MS Sans Serif";
    DialogItemTemplate label;
    label.style = kWsChild | kWsVisible;
    label.x = 8, label.y = 8, label.cx = static_cast<std::int16_t>(width - 16), label.cy = textHeight;
    label.id = 0xFFFF;
    label.className = "static";
    label.text = message;
    dialog.items.push_back(label);
    auto x = static_cast<std::int16_t>((width - buttonsWidth) / 2);
    for (std::size_t i = 0; i < buttons.size(); ++i) {
        DialogItemTemplate button;
        button.style = kWsChild | kWsVisible | kWsTabStop | (i == 0 ? kBsDefPushButton : 0u);
        button.x = x, button.y = static_cast<std::int16_t>(textHeight + 16);
        button.cx = 50, button.cy = 14;
        button.id = buttons[i].first;
        button.className = "button";
        button.text = buttons[i].second;
        dialog.items.push_back(button);
        x = static_cast<std::int16_t>(x + 54);
    }
    return dialog;
}

// ---- the dialog ----

std::unique_ptr<Dialog> Dialog::Create(WindowManager &windows, const DialogTemplate &dialog,
                                       std::uint32_t owner, std::uint32_t instance,
                                       std::uint32_t dialogProc) {
    const Rect client{0, 0, dialogX(dialog.cx), dialogY(dialog.cy)};
    const std::uint32_t style = dialog.style & ~kWsVisible;
    const Rect outer = WindowManager::AdjustRect(client, style, false, dialog.exStyle);
    const std::int32_t width = outer.right - outer.left, height = outer.bottom - outer.top;
    std::int32_t x = 0, y = 0;
    if ((dialog.style & kDsCenter) != 0 || !windows.IsWindow(owner)) {
        const gdi32::ScreenInfo screen = windows.Screen();
        x = (screen.width - width) / 2;
        y = (screen.height - height) / 2;
    } else {
        windows.ClientOrigin(owner, x, y);
        x += dialogX(dialog.x);
        y += dialogY(dialog.y);
    }
    CreateParams params;
    params.exStyle = dialog.exStyle;
    params.className = dialog.className.empty() ? "#32770" : dialog.className;
    params.windowName = dialog.title;
    params.style = style & ~kWsChild; // a modal dialog is a top-level window owned by `owner`
    params.x = x, params.y = y, params.width = width, params.height = height;
    params.parent = owner;
    params.instance = instance;
    const std::uint32_t handle = windows.Create(params);
    if (handle == 0) {
        return nullptr;
    }
    std::unique_ptr<Dialog> made(new Dialog(windows, handle));
    WindowRecord *record = windows.Find(handle);
    record->dialog = true;
    if (record->extra.size() < 3) {
        record->extra.resize(3, 0);
    }
    record->extra[kDwlDlgProc / 4] = dialogProc;
    for (const DialogItemTemplate &item : dialog.items) {
        CreateParams child;
        child.exStyle = item.exStyle;
        child.className = item.className;
        child.windowName = item.text;
        child.style = item.style | kWsChild;
        child.x = dialogX(item.x), child.y = dialogY(item.y);
        child.width = dialogX(item.cx), child.height = dialogY(item.cy);
        child.parent = handle;
        child.menuOrId = item.id;
        child.instance = instance;
        if (windows.FindClass(child.className) == nullptr) {
            child.className = "static"; // drawn as an empty frame (kindOf)
        }
        const std::uint32_t control = windows.Create(child);
        if (control == 0) {
            windows.Destroy(handle);
            return nullptr;
        }
        WindowRecord *controlRecord = windows.Find(control);
        controlRecord->visible = (item.style & kWsVisible) != 0;
        controlRecord->enabled = (item.style & kWsDisabled) == 0;
        if (child.className != item.className) {
            controlRecord->className = item.className;
        }
        made->controls_.push_back(control);
    }
    for (const std::uint32_t control : made->controls_) {
        if (made->focusable(control)) {
            made->focus_ = control;
            break;
        }
    }
    return made;
}

Dialog::Kind Dialog::kindOf(std::uint32_t control) const {
    const WindowRecord *record = windows_.Find(control);
    if (record == nullptr) {
        return Kind::kOther;
    }
    if (record->className == "button") {
        switch (record->style & kBsTypeMask) {
        case kBsPushButton:
        case kBsDefPushButton:
            return Kind::kButton;
        case kBsGroupBox:
            return Kind::kGroupBox;
        default:
            return Kind::kCheckBox; // check boxes and radio buttons alike
        }
    }
    if (record->className == "edit") {
        return Kind::kEdit;
    }
    if (record->className == "static") {
        return Kind::kStatic;
    }
    return Kind::kOther;
}

bool Dialog::focusable(std::uint32_t control) const {
    const WindowRecord *record = windows_.Find(control);
    const Kind kind = kindOf(control);
    return record != nullptr && record->visible && record->enabled &&
           (kind == Kind::kButton || kind == Kind::kCheckBox || kind == Kind::kEdit);
}

void Dialog::SetFocus(std::uint32_t control) {
    if (std::find(controls_.begin(), controls_.end(), control) != controls_.end()) {
        focus_ = control;
    }
}

void Dialog::moveFocus(int step) {
    if (controls_.empty()) {
        return;
    }
    const auto n = static_cast<int>(controls_.size());
    int at = static_cast<int>(std::find(controls_.begin(), controls_.end(), focus_) - controls_.begin());
    if (at >= n) {
        at = step > 0 ? -1 : n;
    }
    for (int tried = 0; tried < n; ++tried) {
        at = ((at + step) % n + n) % n;
        if (focusable(controls_[static_cast<std::size_t>(at)])) {
            focus_ = controls_[static_cast<std::size_t>(at)];
            return;
        }
    }
}

std::uint32_t Dialog::DefaultId() const {
    for (const std::uint32_t control : controls_) {
        const WindowRecord *record = windows_.Find(control);
        if (record->className == "button" && (record->style & kBsTypeMask) == kBsDefPushButton) {
            return record->id;
        }
    }
    return kIdOk;
}

std::optional<Dialog::Command> Dialog::FirstPushButton() const {
    for (const std::uint32_t control : controls_) {
        const WindowRecord *record = windows_.Find(control);
        const std::uint32_t type = record->style & kBsTypeMask;
        if (record->className == "button" && record->enabled &&
            (type == kBsPushButton || type == kBsDefPushButton)) {
            return Command{record->id, kBnClicked, control};
        }
    }
    return std::nullopt;
}

std::uint32_t Dialog::GetCheck(std::uint32_t control) const {
    const auto found = checks_.find(control);
    return found == checks_.end() ? 0 : found->second;
}

void Dialog::SetCheck(std::uint32_t control, std::uint32_t state) {
    checks_[control] = state;
}

void Dialog::SetLimit(std::uint32_t control, std::size_t limit) {
    limits_[control] = limit;
}

std::optional<Dialog::Command> Dialog::press(std::uint32_t control) {
    const WindowRecord *record = windows_.Find(control);
    if (record == nullptr || !record->enabled) {
        return std::nullopt;
    }
    const Kind kind = kindOf(control);
    if (kind == Kind::kCheckBox) {
        const std::uint32_t type = record->style & kBsTypeMask;
        if (type == kBsAutoCheckBox) {
            checks_[control] = GetCheck(control) == 0 ? 1 : 0;
        } else if (type == kBsAutoRadioButton) {
            for (const std::uint32_t other : controls_) {
                const WindowRecord *o = windows_.Find(other);
                if (o->className == "button" && (o->style & kBsTypeMask) == kBsAutoRadioButton) {
                    checks_[other] = 0;
                }
            }
            checks_[control] = 1;
        }
    } else if (kind != Kind::kButton) {
        return std::nullopt;
    }
    return Command{record->id, kBnClicked, control};
}

std::optional<Dialog::Command> Dialog::OnKeyDown(std::uint32_t vk, bool shift) {
    const Kind focused = kindOf(focus_);
    switch (vk) {
    case kVkTab:
        moveFocus(shift ? -1 : 1);
        return std::nullopt;
    case kVkUp:
    case kVkLeft:
        if (focused != Kind::kEdit) {
            moveFocus(-1);
        }
        return std::nullopt;
    case kVkDown:
    case kVkRight:
        if (focused != Kind::kEdit) {
            moveFocus(1);
        }
        return std::nullopt;
    case kVkReturn: {
        // A focused push button is the one Enter presses; otherwise the default.
        if (focused == Kind::kButton) {
            return press(focus_);
        }
        const std::uint32_t id = DefaultId();
        for (const std::uint32_t control : controls_) {
            if (windows_.Find(control)->id == id) {
                return press(control);
            }
        }
        return Command{id, kBnClicked, 0};
    }
    case kVkEscape: {
        for (const std::uint32_t control : controls_) {
            if (windows_.Find(control)->id == kIdCancel) {
                return Command{kIdCancel, kBnClicked, control};
            }
        }
        return Command{kIdCancel, kBnClicked, 0};
    }
    case kVkSpace:
        if (focused == Kind::kButton || focused == Kind::kCheckBox) {
            return press(focus_);
        }
        return std::nullopt;
    case kVkBack:
        if (focused == Kind::kEdit) {
            WindowRecord *record = windows_.Find(focus_);
            if (!record->text.empty()) {
                record->text.pop_back();
                return Command{record->id, kEnChange, focus_};
            }
        }
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

std::optional<Dialog::Command> Dialog::OnChar(char c) {
    if (kindOf(focus_) != Kind::kEdit || static_cast<unsigned char>(c) < 0x20) {
        return std::nullopt;
    }
    WindowRecord *record = windows_.Find(focus_);
    if ((record->style & kEsNumber) != 0 && (c < '0' || c > '9')) {
        return std::nullopt;
    }
    const auto limit = limits_.find(focus_);
    if (limit != limits_.end() && limit->second != 0 && record->text.size() >= limit->second) {
        return std::nullopt;
    }
    record->text += c;
    return Command{record->id, kEnChange, focus_};
}

std::uint32_t Dialog::controlAt(std::int32_t x, std::int32_t y) const {
    for (auto it = controls_.rbegin(); it != controls_.rend(); ++it) {
        const WindowRecord *record = windows_.Find(*it);
        const Rect &r = record->rect;
        if (record->visible && x >= r.left && x < r.right && y >= r.top && y < r.bottom &&
            kindOf(*it) != Kind::kGroupBox) {
            return *it;
        }
    }
    return 0;
}

std::optional<Dialog::Command> Dialog::OnMouse(std::uint32_t message, std::int32_t x,
                                               std::int32_t y) {
    const std::uint32_t under = controlAt(x, y);
    switch (message) {
    case kWmLButtonDown:
        pressed_ = 0;
        if (under != 0 && focusable(under)) {
            focus_ = under;
            pressed_ = under;
            pressedInside_ = true;
        }
        return std::nullopt;
    case kWmMouseMove:
        pressedInside_ = pressed_ != 0 && under == pressed_;
        return std::nullopt;
    case kWmLButtonUp: {
        const std::uint32_t was = std::exchange(pressed_, 0);
        if (was != 0 && under == was) {
            return press(was);
        }
        return std::nullopt;
    }
    default:
        return std::nullopt;
    }
}

// ---- drawing ----

void Dialog::Draw(gdi32::Gdi &gdi, std::uint32_t dc) const {
    const WindowRecord *record = windows_.Find(handle_);
    if (record == nullptr) {
        return;
    }
    const Rect outer{0, 0, record->rect.right - record->rect.left,
                     record->rect.bottom - record->rect.top};
    gdi.FillSolid(dc, outer, kFace);
    const Frame frame = frameOf(record->style, record->exStyle);
    if (frame.side >= 2) {
        frame3d(gdi, dc, outer, kFace, kDark);
        frame3d(gdi, dc, inset(outer, 1), kHighlight, kShadow);
    } else if (frame.side == 1) {
        frame3d(gdi, dc, outer, kDark, kDark);
    }
    gdi.SetBkMode(dc, gdi32::kTransparent);
    if ((record->style & kWsCaption) == kWsCaption) {
        const Rect caption{frame.side, frame.side, outer.right - frame.side,
                           frame.top - 1};
        gdi.FillSolid(dc, caption, kCaption);
        text(gdi, dc, caption.left + 3, caption.top + (caption.bottom - caption.top - 8) / 2,
             record->text, kCaptionText);
    }
    for (const std::uint32_t control : controls_) {
        if (windows_.Find(control)->visible) {
            drawControl(gdi, dc, control, frame.side, frame.top);
        }
    }
}

void Dialog::drawControl(gdi32::Gdi &gdi, std::uint32_t dc, std::uint32_t control,
                         std::int32_t ox, std::int32_t oy) const {
    const WindowRecord *record = windows_.Find(control);
    const Rect r{record->rect.left + ox, record->rect.top + oy, record->rect.right + ox,
                 record->rect.bottom + oy};
    const std::string label = shown(record->text);
    const Colorref ink = record->enabled ? kText : kGreyText;
    const std::int32_t textW = static_cast<std::int32_t>(label.size()) * 6;
    const std::int32_t midY = r.top + (r.bottom - r.top - 8) / 2;
    switch (kindOf(control)) {
    case Kind::kButton: {
        const bool down = pressed_ == control && pressedInside_;
        const bool isDefault = (record->style & kBsTypeMask) == kBsDefPushButton;
        gdi.FillSolid(dc, r, kFace);
        Rect face = r;
        if (isDefault || focus_ == control) {
            frame3d(gdi, dc, r, kDark, kDark);
            face = inset(r, 1);
        }
        if (down) {
            frame3d(gdi, dc, face, kShadow, kShadow);
        } else {
            frame3d(gdi, dc, face, kHighlight, kDark);
            frame3d(gdi, dc, inset(face, 1), kFace, kShadow);
        }
        const std::int32_t shift = down ? 1 : 0;
        text(gdi, dc, r.left + (r.right - r.left - textW) / 2 + shift, midY + shift, label, ink);
        break;
    }
    case Kind::kCheckBox: {
        const Rect box{r.left, midY - 1, r.left + 10, midY + 9};
        gdi.FillSolid(dc, box, kWindowBack);
        frame3d(gdi, dc, box, kShadow, kHighlight);
        if (GetCheck(control) != 0) {
            gdi.FillSolid(dc, inset(box, 3), kText);
        }
        text(gdi, dc, r.left + 14, midY, label, ink);
        break;
    }
    case Kind::kGroupBox:
        frame3d(gdi, dc, Rect{r.left, r.top + 4, r.right, r.bottom}, kShadow, kHighlight);
        gdi.FillSolid(dc, Rect{r.left + 6, r.top, r.left + 10 + textW, r.top + 8}, kFace);
        text(gdi, dc, r.left + 8, r.top, label, ink);
        break;
    case Kind::kEdit: {
        gdi.FillSolid(dc, r, kWindowBack);
        frame3d(gdi, dc, r, kShadow, kHighlight);
        frame3d(gdi, dc, inset(r, 1), kDark, kFace);
        const std::string value = (record->style & kEsPassword) != 0
                                      ? std::string(record->text.size(), '*')
                                      : record->text;
        text(gdi, dc, r.left + 3, midY, value, ink);
        if (focus_ == control) {
            const std::int32_t caret = r.left + 3 + static_cast<std::int32_t>(value.size()) * 6;
            gdi.FillSolid(dc, Rect{caret, midY - 1, caret + 1, midY + 9}, kText);
        }
        break;
    }
    case Kind::kStatic: {
        std::int32_t y = r.top;
        std::size_t start = 0;
        while (start <= label.size()) {
            const std::size_t end = std::min(label.find('\n', start), label.size());
            const std::string line = label.substr(start, end - start);
            const auto w = static_cast<std::int32_t>(line.size()) * 6;
            std::int32_t x = r.left;
            if ((record->style & 0x0F) == kSsCenter) {
                x = r.left + (r.right - r.left - w) / 2;
            } else if ((record->style & 0x0F) == kSsRight) {
                x = r.right - w;
            }
            text(gdi, dc, x, y, line, ink);
            y += 8;
            start = end + 1;
        }
        break;
    }
    case Kind::kOther:
        frame3d(gdi, dc, r, kShadow, kHighlight);
        break;
    }
}

} // namespace sfcop::hle::user32
