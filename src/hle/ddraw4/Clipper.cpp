#include "hle/ddraw4/Clipper.h"
#include "hle/ddraw4/DirectDraw.h"

#include <algorithm>

namespace sfcop::hle::ddraw4 {

Clipper::Clipper(DirectDraw *owner) : owner_(owner) {
    if (owner_ != nullptr) {
        owner_->AdoptChild();
    }
}

Clipper::~Clipper() {
    if (owner_ != nullptr) {
        owner_->ChildGone();
    }
}

HResult Clipper::SetHWnd(std::uint32_t flags, std::uint32_t window) {
    if (flags != 0) {
        return kDdErrInvalidParams;
    }
    window_ = window;
    return kDdOk;
}

HResult Clipper::GetHWnd(std::uint32_t &window) const {
    window = window_;
    return kDdOk;
}

HResult Clipper::SetClipList(const std::vector<RectL> *rects, std::uint32_t) {
    hasList_ = rects != nullptr;
    list_ = hasList_ ? *rects : std::vector<RectL>{};
    return kDdOk;
}

HResult Clipper::GetClipList(const RectL *clip, std::vector<RectL> &out) const {
    std::vector<RectL> rects;
    if (hasList_) {
        rects = list_;
    } else if (window_ != 0) {
        const DisplayModeState mode = owner_ != nullptr ? owner_->Mode() : DisplayModeState{};
        rects.push_back(RectL{0, 0, static_cast<std::int32_t>(mode.width),
                              static_cast<std::int32_t>(mode.height)});
    } else {
        return kDdErrNoClipList;
    }
    out.clear();
    for (RectL rect : rects) {
        if (clip != nullptr) {
            rect.left = std::max(rect.left, clip->left);
            rect.top = std::max(rect.top, clip->top);
            rect.right = std::min(rect.right, clip->right);
            rect.bottom = std::min(rect.bottom, clip->bottom);
        }
        if (rect.left < rect.right && rect.top < rect.bottom) {
            out.push_back(rect);
        }
    }
    return kDdOk;
}

HResult Clipper::IsClipListChanged(bool &changed) const {
    changed = false;
    return kDdOk;
}

} // namespace sfcop::hle::ddraw4
