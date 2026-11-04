// IDirectDrawClipper. Bookkeeping only: it remembers its window or clip list and answers
// questions about them. Blt clips to the destination surface when a clipper is attached; the
// window system does the rest of the clipping the original's clipper did.
#pragma once

#include "hle/ddraw4/ComObject.h"
#include "hle/ddraw4/Ddraw4Defs.h"

#include <vector>

namespace sfcop::hle::ddraw4 {

class DirectDraw;

class Clipper final : public ComObject {
public:
    // `owner` is the DirectDraw object that created it, or nullptr for DirectDrawCreateClipper.
    explicit Clipper(DirectDraw *owner);

    // Clips to a window, given by the game's handle value. A non-zero `flags` is
    // DDERR_INVALIDPARAMS, as the original documents no flag here.
    HResult SetHWnd(std::uint32_t flags, std::uint32_t window);
    HResult GetHWnd(std::uint32_t &window) const;

    // Replaces the clip list (nullptr removes it).
    HResult SetClipList(const std::vector<RectL> *rects, std::uint32_t flags);

    // The clip list, intersected with `clip` when one is given: the list set by SetClipList(), or
    // with a window, the display's rectangle. DDERR_NOCLIPLIST with neither.
    HResult GetClipList(const RectL *clip, std::vector<RectL> &out) const;

    // Whether the clip list changed since it was last read: never, since nothing moves windows.
    HResult IsClipListChanged(bool &changed) const;

    HResult Initialize() const { return kDdErrAlreadyInitialized; }

private:
    ~Clipper() override;

    DirectDraw *owner_;
    std::uint32_t window_ = 0;
    bool hasList_ = false;
    std::vector<RectL> list_;
};

} // namespace sfcop::hle::ddraw4
