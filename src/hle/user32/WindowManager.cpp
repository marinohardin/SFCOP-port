#include "hle/user32/WindowManager.h"

#include "hle/user32/User32Defs.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <memory>

namespace sfcop::hle::user32 {

namespace {

std::string lower(std::string text) {
    for (char &c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

std::int32_t width(const Rect &r) {
    return r.right - r.left;
}

std::int32_t height(const Rect &r) {
    return r.bottom - r.top;
}

constexpr std::uint32_t kMfByPosition = 0x0400;
constexpr std::uint32_t kMfChecked = 0x0008;

} // namespace

WindowHost countingUserHandles(std::function<gdi32::ScreenInfo()> screen) {
    auto next = std::make_shared<std::array<std::uint32_t, 3>>(
        std::array<std::uint32_t, 3>{0x3000u, 0x4000u, 0x5000u});
    return WindowHost{std::move(screen),
                      [next](UserHandle kind) {
                          return (*next)[static_cast<std::size_t>(kind)] += 4;
                      },
                      [](std::uint32_t) {}, [] { return true; }};
}

Frame frameOf(std::uint32_t style, std::uint32_t exStyle) {
    Frame frame;
    if ((style & kWsThickFrame) != 0) {
        frame.side = kFrameSize;
    } else if ((style & kWsDlgFrame) != 0 || (exStyle & kWsExDlgModalFrame) != 0) {
        frame.side = kDlgFrameSize;
    } else if ((style & kWsBorder) != 0) {
        frame.side = kBorderSize;
    }
    frame.top = frame.side;
    if ((style & kWsCaption) == kWsCaption) {
        frame.top += kCaptionHeight;
    }
    return frame;
}

WindowManager::WindowManager(platform::Window &window, WindowHost host)
    : window_(window), host_(std::move(host)) {
    // The system classes the dialogs use, run by this module, with atoms outside the range
    // RegisterClassA gives: the dialog class's is 0x8002, so that "#32770" names it.
    const std::pair<const char *, std::uint32_t> system[] = {
        {"#32770", kDialogProc}, {"button", kButtonProc}, {"edit", kEditProc}, {"static", kStaticProc}};
    const std::uint16_t atoms[] = {0x8002, 0x8080, 0x8081, 0x8082};
    for (std::size_t i = 0; i < 4; ++i) {
        WindowClass made;
        made.name = system[i].first;
        made.proc = system[i].second;
        made.windowExtra = made.proc == kDialogProc ? 30 : 0; // DLGWINDOWEXTRA
        made.atom = atoms[i];
        classes_.emplace(made.atom, made);
    }
}

// ---- classes ----

std::uint16_t WindowManager::RegisterClass(const WindowClass &windowClass) {
    const std::string name = lower(windowClass.name);
    if (name.empty() || FindClass(name) != nullptr) {
        return 0;
    }
    WindowClass made = windowClass;
    made.name = name;
    made.atom = nextAtom_++;
    classes_.emplace(made.atom, made);
    return made.atom;
}

const WindowClass *WindowManager::FindClass(const std::string &nameOrAtom) const {
    if (nameOrAtom.size() > 1 && nameOrAtom[0] == '#') {
        const auto atom = static_cast<std::uint16_t>(std::strtoul(nameOrAtom.c_str() + 1, nullptr, 10));
        const auto found = classes_.find(atom);
        return found == classes_.end() ? nullptr : &found->second;
    }
    const std::string name = lower(nameOrAtom);
    for (const auto &entry : classes_) {
        if (entry.second.name == name) {
            return &entry.second;
        }
    }
    return nullptr;
}

// ---- windows ----

std::uint32_t WindowManager::Create(const CreateParams &params) {
    const WindowClass *windowClass = FindClass(params.className);
    if (windowClass == nullptr) {
        return 0;
    }
    if ((params.style & kWsChild) != 0 && Find(params.parent) == nullptr) {
        return 0;
    }
    const std::uint32_t handle = host_.allocate(UserHandle::kWindow);
    if (handle == 0) {
        return 0;
    }
    WindowRecord record;
    record.handle = handle;
    record.className = windowClass->name;
    record.proc = windowClass->proc;
    record.style = params.style & ~kWsVisible;
    record.exStyle = params.exStyle;
    record.parent = params.parent;
    record.id = params.menuOrId;
    record.instance = params.instance;
    record.text = params.windowName;
    record.dialog = windowClass->proc == kDialogProc;
    record.extra.assign(static_cast<std::size_t>(std::max(windowClass->windowExtra, 0) + 3) / 4, 0);
    const gdi32::ScreenInfo screen = host_.screen();
    std::int32_t x = params.x, y = params.y, w = params.width, h = params.height;
    if (x == kCwUseDefault) {
        x = 0, y = 0;
    }
    if (w == kCwUseDefault) {
        w = screen.width * 3 / 4, h = screen.height * 3 / 4;
    }
    record.rect = Rect{x, y, x + std::max(w, 0), y + std::max(h, 0)};
    windows_.emplace(handle, std::move(record));
    order_.push_back(handle);
    return handle;
}

std::vector<std::uint32_t> WindowManager::Destroy(std::uint32_t handle) {
    std::vector<std::uint32_t> removed;
    if (Find(handle) == nullptr) {
        return removed;
    }
    for (const std::uint32_t child : Children(handle)) {
        const std::vector<std::uint32_t> below = Destroy(child);
        removed.insert(removed.end(), below.begin(), below.end());
    }
    // Owned top-level windows go with their owner, as children do.
    for (const std::uint32_t other : TopLevelWindows()) {
        if (Find(other) != nullptr && Find(other)->parent == handle) {
            const std::vector<std::uint32_t> owned = Destroy(other);
            removed.insert(removed.end(), owned.begin(), owned.end());
        }
    }
    if (active_ == handle) {
        active_ = 0;
    }
    if (focus_ == handle) {
        focus_ = 0;
    }
    windows_.erase(handle);
    order_.erase(std::remove(order_.begin(), order_.end(), handle), order_.end());
    host_.release(handle);
    removed.push_back(handle);
    return removed;
}

WindowRecord *WindowManager::Find(std::uint32_t handle) {
    const auto found = windows_.find(handle);
    return found == windows_.end() ? nullptr : &found->second;
}

const WindowRecord *WindowManager::Find(std::uint32_t handle) const {
    const auto found = windows_.find(handle);
    return found == windows_.end() ? nullptr : &found->second;
}

std::vector<std::uint32_t> WindowManager::Children(std::uint32_t parent) const {
    std::vector<std::uint32_t> children;
    for (const std::uint32_t handle : order_) {
        const WindowRecord *record = Find(handle);
        if ((record->style & kWsChild) != 0 && record->parent == parent) {
            children.push_back(handle);
        }
    }
    return children;
}

std::uint32_t WindowManager::ChildWithId(std::uint32_t parent, std::uint32_t id) const {
    for (const std::uint32_t child : Children(parent)) {
        if (Find(child)->id == id) {
            return child;
        }
    }
    return 0;
}

std::vector<std::uint32_t> WindowManager::TopLevelWindows() const {
    std::vector<std::uint32_t> top;
    for (const std::uint32_t handle : order_) {
        if ((Find(handle)->style & kWsChild) == 0) {
            top.push_back(handle);
        }
    }
    return top;
}

std::uint32_t WindowManager::FindTopLevel(const std::string &className,
                                          const std::string &title) const {
    const std::string wanted = lower(className);
    for (const std::uint32_t handle : TopLevelWindows()) {
        const WindowRecord *record = Find(handle);
        if ((wanted.empty() || record->className == wanted) &&
            (title.empty() || record->text == title)) {
            return handle;
        }
    }
    return 0;
}

// ---- geometry ----

Frame WindowManager::frameWithMenu(const WindowRecord &record) const {
    Frame frame = frameOf(record.style, record.exStyle);
    if ((record.style & kWsChild) == 0 && IsMenu(record.id)) {
        frame.top += kMenuBarHeight;
    }
    return frame;
}

Rect WindowManager::WindowRect(std::uint32_t handle) const {
    const WindowRecord *record = Find(handle);
    if (record == nullptr) {
        return Rect{};
    }
    Rect rect = record->rect;
    if ((record->style & kWsChild) != 0) {
        std::int32_t x = 0, y = 0;
        ClientOrigin(record->parent, x, y);
        rect.left += x, rect.right += x, rect.top += y, rect.bottom += y;
    }
    return rect;
}

Rect WindowManager::ClientRect(std::uint32_t handle) const {
    const WindowRecord *record = Find(handle);
    if (record == nullptr) {
        return Rect{};
    }
    const Frame frame = frameWithMenu(*record);
    return Rect{0, 0, std::max(width(record->rect) - 2 * frame.side, 0),
                std::max(height(record->rect) - frame.top - frame.side, 0)};
}

void WindowManager::ClientOrigin(std::uint32_t handle, std::int32_t &x, std::int32_t &y) const {
    const WindowRecord *record = Find(handle);
    if (record == nullptr) {
        x = y = 0;
        return;
    }
    const Rect rect = WindowRect(handle);
    const Frame frame = frameWithMenu(*record);
    x = rect.left + frame.side;
    y = rect.top + frame.top;
}

Rect WindowManager::AdjustRect(const Rect &client, std::uint32_t style, bool menu,
                               std::uint32_t exStyle) {
    const Frame frame = frameOf(style, exStyle);
    return Rect{client.left - frame.side, client.top - frame.top - (menu ? kMenuBarHeight : 0),
                client.right + frame.side, client.bottom + frame.side};
}

bool WindowManager::SetPosition(std::uint32_t handle, std::int32_t x, std::int32_t y,
                                std::int32_t w, std::int32_t h, std::uint32_t flags) {
    WindowRecord *record = Find(handle);
    if (record == nullptr) {
        return false;
    }
    Rect &rect = record->rect;
    if ((flags & kSwpNoMove) == 0) {
        const std::int32_t keepW = width(rect), keepH = height(rect);
        rect.left = x, rect.top = y, rect.right = x + keepW, rect.bottom = y + keepH;
    }
    if ((flags & kSwpNoSize) == 0) {
        rect.right = rect.left + std::max(w, 0);
        rect.bottom = rect.top + std::max(h, 0);
    }
    return true;
}

// ---- long values ----

std::optional<std::uint32_t> WindowManager::GetLong(std::uint32_t handle,
                                                    std::int32_t index) const {
    const WindowRecord *record = Find(handle);
    if (record == nullptr) {
        return std::nullopt;
    }
    switch (index) {
    case kGwlWndProc:
        return record->proc;
    case kGwlHInstance:
        return record->instance;
    case kGwlHwndParent:
        return record->parent;
    case kGwlId:
        return record->id;
    case kGwlStyle:
        return record->style | (record->visible ? kWsVisible : 0u);
    case kGwlExStyle:
        return record->exStyle;
    case kGwlUserData:
        return record->userData;
    default:
        break;
    }
    if (index < 0 || index % 4 != 0 || static_cast<std::size_t>(index / 4) >= record->extra.size()) {
        return std::nullopt;
    }
    return record->extra[static_cast<std::size_t>(index / 4)];
}

std::optional<std::uint32_t> WindowManager::SetLong(std::uint32_t handle, std::int32_t index,
                                                    std::uint32_t value) {
    WindowRecord *record = Find(handle);
    const std::optional<std::uint32_t> previous = GetLong(handle, index);
    if (record == nullptr || !previous) {
        return std::nullopt;
    }
    switch (index) {
    case kGwlWndProc:
        record->proc = value;
        break;
    case kGwlHInstance:
        record->instance = value;
        break;
    case kGwlHwndParent:
        record->parent = value;
        break;
    case kGwlId:
        record->id = value;
        break;
    case kGwlStyle:
        record->style = value & ~kWsVisible;
        record->visible = (value & kWsVisible) != 0;
        break;
    case kGwlExStyle:
        record->exStyle = value;
        break;
    case kGwlUserData:
        record->userData = value;
        break;
    default:
        record->extra[static_cast<std::size_t>(index / 4)] = value;
        break;
    }
    return previous;
}

// ---- text, visibility, focus ----

bool WindowManager::SetText(std::uint32_t handle, const std::string &text) {
    WindowRecord *record = Find(handle);
    if (record == nullptr) {
        return false;
    }
    record->text = text;
    if (handle == active_ && !text.empty() && host_.onMainThread()) {
        static_cast<void>(window_.SetTitle(text).Ok()); // a headless window has no title
    }
    return true;
}

bool WindowManager::SetVisible(std::uint32_t handle, bool visible) {
    WindowRecord *record = Find(handle);
    if (record == nullptr) {
        return false;
    }
    const bool was = record->visible;
    record->visible = visible;
    if (visible && !was) {
        Invalidate(handle, nullptr);
    }
    return was;
}

std::uint32_t WindowManager::SetActive(std::uint32_t handle) {
    const std::uint32_t previous = active_;
    const std::uint32_t top = TopLevelOf(handle);
    active_ = top;
    if (top != 0 && top != previous) {
        const WindowRecord *record = Find(top);
        if (!record->text.empty() && host_.onMainThread()) {
            static_cast<void>(window_.SetTitle(record->text).Ok());
        }
    }
    return previous;
}

std::uint32_t WindowManager::SetFocus(std::uint32_t handle) {
    const std::uint32_t previous = focus_;
    if (handle == 0 || Find(handle) != nullptr) {
        focus_ = handle;
    }
    return previous;
}

std::uint32_t WindowManager::TopLevelOf(std::uint32_t handle) const {
    const WindowRecord *record = Find(handle);
    while (record != nullptr && (record->style & kWsChild) != 0) {
        const WindowRecord *parent = Find(record->parent);
        if (parent == nullptr) {
            break;
        }
        record = parent;
    }
    return record == nullptr ? 0 : record->handle;
}

bool WindowManager::IsIconic(std::uint32_t handle) {
    const WindowRecord *record = Find(handle);
    if (record == nullptr) {
        return false;
    }
    if ((record->style & kWsMinimize) != 0) {
        return true;
    }
    if (TopLevelOf(handle) != active_) {
        return false;
    }
    const auto state = window_.GetState();
    return state.Ok() && state.Value().minimized;
}

// ---- painting ----

void WindowManager::Invalidate(std::uint32_t handle, const Rect *rect) {
    WindowRecord *record = Find(handle);
    if (record == nullptr) {
        return;
    }
    const Rect client = ClientRect(handle);
    const Rect area = rect != nullptr ? *rect : client;
    if (!record->invalid) {
        record->invalidRect = area;
    } else {
        Rect &u = record->invalidRect;
        u.left = std::min(u.left, area.left), u.top = std::min(u.top, area.top);
        u.right = std::max(u.right, area.right), u.bottom = std::max(u.bottom, area.bottom);
    }
    record->invalid = true;
}

void WindowManager::Validate(std::uint32_t handle) {
    if (WindowRecord *record = Find(handle)) {
        record->invalid = false;
        record->invalidRect = Rect{};
    }
}

std::uint32_t WindowManager::NextInvalid(std::uint32_t filter) const {
    for (const std::uint32_t handle : order_) {
        const WindowRecord *record = Find(handle);
        if (!record->invalid || !record->visible || record->destroying) {
            continue;
        }
        if (filter == 0 || handle == filter || TopLevelOf(handle) == filter) {
            return handle;
        }
    }
    return 0;
}

// ---- menus ----

std::uint32_t WindowManager::CreateMenu() {
    const std::uint32_t handle = host_.allocate(UserHandle::kMenu);
    if (handle != 0) {
        menus_[handle];
    }
    return handle;
}

bool WindowManager::AppendMenu(std::uint32_t menu, std::uint32_t flags, std::uint32_t id,
                               std::string text) {
    const auto found = menus_.find(menu);
    if (found == menus_.end()) {
        return false;
    }
    found->second.push_back(MenuItem{flags, id, std::move(text)});
    return true;
}

std::uint32_t WindowManager::CheckMenuItem(std::uint32_t menu, std::uint32_t item,
                                           std::uint32_t flags) {
    const auto found = menus_.find(menu);
    if (found == menus_.end()) {
        return 0xFFFFFFFFu;
    }
    std::vector<MenuItem> &items = found->second;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const bool match = (flags & kMfByPosition) != 0 ? i == item : items[i].id == item;
        if (match) {
            const std::uint32_t previous = items[i].flags & kMfChecked;
            items[i].flags = (items[i].flags & ~kMfChecked) | (flags & kMfChecked);
            return previous;
        }
    }
    return 0xFFFFFFFFu;
}

bool WindowManager::DeleteMenu(std::uint32_t menu, std::uint32_t item, std::uint32_t flags) {
    const auto found = menus_.find(menu);
    if (found == menus_.end()) {
        return false;
    }
    std::vector<MenuItem> &items = found->second;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if ((flags & kMfByPosition) != 0 ? i == item : items[i].id == item) {
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

std::uint32_t WindowManager::SystemMenu(std::uint32_t handle) {
    if (Find(handle) == nullptr) {
        return 0;
    }
    std::uint32_t &menu = systemMenus_[handle];
    if (menu == 0) {
        menu = CreateMenu();
        AppendMenu(menu, 0, kScClose, "&Close");
    }
    return menu;
}

const std::vector<MenuItem> *WindowManager::MenuItems(std::uint32_t menu) const {
    const auto found = menus_.find(menu);
    return found == menus_.end() ? nullptr : &found->second;
}

// ---- clipboard ----

bool WindowManager::OpenClipboard(std::uint32_t) {
    if (clipboardOpen_) {
        return false;
    }
    clipboardOpen_ = true;
    return true;
}

bool WindowManager::CloseClipboard() {
    const bool was = clipboardOpen_;
    clipboardOpen_ = false;
    return was;
}

bool WindowManager::EmptyClipboard() {
    if (!clipboardOpen_) {
        return false;
    }
    clipboard_.clear();
    return true;
}

bool WindowManager::SetClipboard(std::uint32_t format, std::vector<std::uint8_t> data) {
    if (!clipboardOpen_) {
        return false;
    }
    clipboard_[format] = std::move(data);
    return true;
}

const std::vector<std::uint8_t> *WindowManager::Clipboard(std::uint32_t format) const {
    const auto found = clipboard_.find(format);
    return !clipboardOpen_ || found == clipboard_.end() ? nullptr : &found->second;
}

// ---- hooks ----

std::uint32_t WindowManager::AddHook(std::int32_t type, std::uint32_t proc) {
    const std::uint32_t handle = host_.allocate(UserHandle::kHook);
    if (handle != 0) {
        hooks_.emplace(handle, Hook{type, proc});
    }
    return handle;
}

bool WindowManager::RemoveHook(std::uint32_t hook) {
    if (hooks_.erase(hook) == 0) {
        return false;
    }
    host_.release(hook);
    return true;
}

// ---- cursor ----

std::int32_t WindowManager::ShowCursor(bool show) {
    const bool wasShown = cursorCount_ >= 0;
    cursorCount_ += show ? 1 : -1;
    const bool shown = cursorCount_ >= 0;
    if (shown != wasShown && host_.onMainThread()) {
        static_cast<void>(window_.SetCursorVisible(shown).Ok()); // no cursor headless
    }
    return cursorCount_;
}

std::uint32_t WindowManager::SetCursor(std::uint32_t cursor) {
    return std::exchange(cursor_, cursor);
}

} // namespace sfcop::hle::user32
