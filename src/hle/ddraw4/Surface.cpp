#include "hle/ddraw4/Surface.h"
#include "hle/ddraw4/Clipper.h"
#include "hle/ddraw4/DirectDraw.h"
#include "hle/ddraw4/Palette.h"

#include <algorithm>
#include <cstring>

namespace sfcop::hle::ddraw4 {

using platform::Error;
using platform::PixelFormat;

namespace {

constexpr std::uint32_t kDdscaps2MipMapSubLevel = 0x00010000;
constexpr std::uint32_t kDdckeyOverlay = 0x00000004 | 0x00000010;

platform::Rect toRect(const RectL &rect) {
    return platform::Rect{rect.left, rect.top, static_cast<std::uint32_t>(rect.right - rect.left),
                          static_cast<std::uint32_t>(rect.bottom - rect.top)};
}

bool isEmpty(const RectL &rect) {
    return rect.left >= rect.right || rect.top >= rect.bottom;
}

bool inside(const RectL &rect, const RectL &bounds) {
    return !isEmpty(rect) && rect.left >= bounds.left && rect.top >= bounds.top &&
           rect.right <= bounds.right && rect.bottom <= bounds.bottom;
}

// Whether `outer` holds all of `inner`.
bool contains(const RectL &outer, const RectL &inner) {
    return inner.left >= outer.left && inner.top >= outer.top && inner.right <= outer.right &&
           inner.bottom <= outer.bottom;
}

RectL unite(const RectL &a, const RectL &b) {
    return RectL{std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right),
                 std::max(a.bottom, b.bottom)};
}

// The bits a raw pixel of `format` can hold.
std::uint32_t formatMask(PixelFormat format) {
    switch (pixelBytes(format)) {
    case 1:
        return 0xFF;
    case 2:
        return 0xFFFF;
    default:
        return format == PixelFormat::kXrgb8888 ? 0x00FFFFFFu : 0xFFFFFFFFu;
    }
}

std::uint32_t readPixel(const std::uint8_t *at, std::uint32_t bytes) {
    std::uint32_t value = 0;
    std::memcpy(&value, at, bytes); // the host is little-endian, as the game's pixels are
    return value;
}

void writePixel(std::uint8_t *at, std::uint32_t bytes, std::uint32_t value) {
    std::memcpy(at, &value, bytes);
}

std::uint32_t pitchOf(std::uint32_t width, PixelFormat format) {
    return (width * pixelBytes(format) + 3u) & ~3u;
}

Surface::HandleGoneHook g_handleGone = nullptr;

} // namespace

void Surface::SetHandleGoneHook(HandleGoneHook hook) {
    g_handleGone = hook;
}

Surface::Surface(DirectDraw &owner, SurfaceRole role, const SurfaceDesc &desc, PixelFormat format,
                 Surface *root, std::uint8_t level)
    : ComObject(root != nullptr ? 0 : 1), owner_(owner), role_(role), desc_(desc), format_(format),
      root_(root), level_(level) {
    owner_.AdoptChild();
    if ((desc.flags & kDdsdCkSrcBlt) != 0) {
        sourceKey_ = desc.srcBlt;
    }
    if ((desc.flags & kDdsdCkDestBlt) != 0) {
        destinationKey_ = desc.destBlt;
    }

    std::uint32_t caps = (desc.flags & kDdsdCaps) != 0 ? desc.caps.caps : 0;
    if ((caps & kDdscapsSystemMemory) == 0) {
        caps |= kDdscapsVideoMemory | kDdscapsLocalVidMem;
    }
    switch (role) {
    case SurfaceRole::kPrimary:
        caps |= kDdscapsVisible | ((caps & kDdscapsFlip) != 0 ? kDdscapsFrontBuffer : 0);
        break;
    case SurfaceRole::kBackBuffer:
        caps = (caps & ~(kDdscapsPrimarySurface | kDdscapsVisible | kDdscapsFrontBuffer)) |
               kDdscapsBackBuffer;
        break;
    case SurfaceRole::kPlain:
        if ((caps & kDdscaps3dDevice) == 0) {
            caps |= kDdscapsOffscreenPlain;
        }
        break;
    case SurfaceRole::kTexture:
    case SurfaceRole::kZBuffer:
        break;
    }
    desc_ = SurfaceDesc{};
    desc_.flags = kDdsdCaps | kDdsdHeight | kDdsdWidth | kDdsdPitch | kDdsdPixelFormat;
    desc_.width = desc.width;
    desc_.height = desc.height;
    desc_.pitch = static_cast<std::int32_t>(pitchOf(desc.width, format));
    desc_.pixelFormat = describe(format);
    desc_.caps =
        SurfaceCaps{caps, desc.caps.caps2 | (level != 0 ? kDdscaps2MipMapSubLevel : 0), 0, 0};
    desc_.textureStage = desc.textureStage;
    pixels_ = owner_.Memory().Allocate(static_cast<std::size_t>(desc_.pitch) * desc_.height);
}

Surface::~Surface() {
    for (Surface *member : chain_) {
        member->root_ = nullptr;
        delete member; // a back buffer or mipmap level lives as long as its root (the header)
    }
    for (Surface *surface : added_) {
        surface->Release();
    }
    if (palette_ != nullptr) {
        palette_->Release();
    }
    if (clipper_ != nullptr) {
        clipper_->Release();
    }
    if (texture_.Valid()) {
        (void)owner_.Device().DestroyTexture(texture_);
    }
    if (patchTexture_.Valid()) {
        (void)owner_.Device().DestroyTexture(patchTexture_);
    }
    owner_.Memory().Free(pixels_);
    if (textureHandle_ != 0 && g_handleGone != nullptr) {
        g_handleGone(textureHandle_);
    }
    owner_.SurfaceGone(*this);
    owner_.ChildGone(); // last: it may destroy the DirectDraw object
}

RectL Surface::bounds() const {
    return RectL{0, 0, static_cast<std::int32_t>(desc_.width),
                 static_cast<std::int32_t>(desc_.height)};
}

bool Surface::isFlipFront() const {
    return !chain_.empty() && role_ != SurfaceRole::kTexture;
}

HResult Surface::Lock(const RectL *rect, SurfaceDesc &out, std::uint32_t flags) {
    if (locked_ || dcOut_) {
        return kDdErrSurfaceBusy;
    }
    const RectL area = rect != nullptr ? *rect : bounds();
    if (!inside(area, bounds())) {
        return kDdErrInvalidRect;
    }
    SyncToHost();
    locked_ = true;
    lockReadOnly_ = (flags & kDdlockReadOnly) != 0;
    lockArea_ = area;
    if (!lockReadOnly_) {
        beginHostEdit(area);
    }
    (void)GetSurfaceDesc(out);
    out.flags |= kDdsdLpSurface;
    out.surface = pixels_ + static_cast<std::size_t>(area.top) * desc_.pitch +
                  static_cast<std::size_t>(area.left) * pixelBytes(format_);
    return kDdOk;
}

HResult Surface::Unlock() {
    if (!locked_) {
        return kDdErrNotLocked;
    }
    locked_ = false;
    if (lockReadOnly_) {
        return kDdOk;
    }
    endHostEdit(lockArea_);
    return afterWrite();
}

HResult Surface::Blt(const RectL *destination, Surface *source, const RectL *sourceRect,
                     std::uint32_t flags, const BltFx *fx) {
    if (locked_ || dcOut_) {
        return kDdErrSurfaceBusy;
    }
    bool fillOperation = false;
    std::uint32_t value = 0;
    if ((flags & kDdbltColorFill) != 0) {
        if (fx == nullptr) {
            return kDdErrInvalidParams;
        }
        fillOperation = true;
        value = fx->fillColor & formatMask(format_);
    } else if ((flags & kDdbltRop) != 0) {
        if (fx == nullptr) {
            return kDdErrInvalidParams;
        }
        if (fx->rop == kRopBlackness || fx->rop == kRopWhiteness) {
            fillOperation = true;
            value = fx->rop == kRopBlackness ? 0 : formatMask(format_);
        } else if (fx->rop != kRopSrcCopy) {
            return kDdErrUnsupported;
        }
    }
    if (((flags & kDdbltDdfx) != 0 && (fx == nullptr || fx->ddfx != 0)) ||
        (flags & (kDdbltKeyDest | kDdbltKeyDestOverride)) != 0) {
        return kDdErrUnsupported;
    }

    RectL area = destination != nullptr ? *destination : bounds();
    if (isEmpty(area)) {
        return kDdErrInvalidRect;
    }
    RectL from;
    if (!fillOperation) {
        if (source == nullptr) {
            return kDdErrInvalidParams;
        }
        if (source != this && (source->locked_ || source->dcOut_)) {
            return kDdErrSurfaceBusy;
        }
        from = sourceRect != nullptr ? *sourceRect : source->bounds();
        if (!inside(from, source->bounds())) {
            return kDdErrInvalidRect;
        }
    }
    if (!inside(area, bounds())) {
        if (clipper_ == nullptr) {
            return kDdErrInvalidRect;
        }
        // Clip to the surface; the source shrinks in proportion.
        const RectL clipped{std::max(area.left, 0), std::max(area.top, 0),
                            std::min(area.right, bounds().right),
                            std::min(area.bottom, bounds().bottom)};
        if (isEmpty(clipped)) {
            return kDdOk;
        }
        if (!fillOperation) {
            const std::int64_t sw = from.right - from.left, sh = from.bottom - from.top;
            const std::int64_t dw = area.right - area.left, dh = area.bottom - area.top;
            from = RectL{
                static_cast<std::int32_t>(from.left + (clipped.left - area.left) * sw / dw),
                static_cast<std::int32_t>(from.top + (clipped.top - area.top) * sh / dh),
                static_cast<std::int32_t>(from.right - (area.right - clipped.right) * sw / dw),
                static_cast<std::int32_t>(from.bottom - (area.bottom - clipped.bottom) * sh / dh)};
            if (isEmpty(from)) {
                return kDdOk;
            }
        }
        area = clipped;
    }
    if (fillOperation) {
        return fill(area, value);
    }

    std::optional<std::uint32_t> key;
    bool keyOverride = false;
    if ((flags & kDdbltKeySrcOverride) != 0) {
        if (fx == nullptr) {
            return kDdErrInvalidParams;
        }
        key = fx->srcKey.low & formatMask(source->format_);
        keyOverride = true;
    } else if ((flags & kDdbltKeySrc) != 0 && source->sourceKey_) {
        key = source->sourceKey_->low & formatMask(source->format_);
    }
    return copy(*source, from, area, key, keyOverride);
}

HResult Surface::BltFast(std::uint32_t x, std::uint32_t y, Surface *source, const RectL *sourceRect,
                         std::uint32_t transfer) {
    if (source == nullptr) {
        return kDdErrInvalidParams;
    }
    if (locked_ || dcOut_ || (source != this && (source->locked_ || source->dcOut_))) {
        return kDdErrSurfaceBusy;
    }
    if ((transfer & kDdbltfastDestColorKey) != 0) {
        return kDdErrUnsupported;
    }
    const RectL from = sourceRect != nullptr ? *sourceRect : source->bounds();
    if (!inside(from, source->bounds()) || x > desc_.width || y > desc_.height) {
        return kDdErrInvalidRect;
    }
    const RectL area{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y),
                     static_cast<std::int32_t>(x) + (from.right - from.left),
                     static_cast<std::int32_t>(y) + (from.bottom - from.top)};
    if (!inside(area, bounds())) {
        return kDdErrInvalidRect;
    }
    std::optional<std::uint32_t> key;
    if ((transfer & kDdbltfastSrcColorKey) != 0 && source->sourceKey_) {
        key = source->sourceKey_->low & formatMask(source->format_);
    }
    return copy(*source, from, area, key, false);
}

HResult Surface::Flip(Surface *target, std::uint32_t) {
    if (!isFlipFront()) {
        return kDdErrNotFlippable;
    }
    Surface *back = chain_.front();
    if (target != nullptr) {
        if (std::find(chain_.begin(), chain_.end(), target) == chain_.end()) {
            return kDdErrInvalidParams;
        }
        back = target;
    }
    if (locked_ || dcOut_ || back->locked_ || back->dcOut_) {
        return kDdErrSurfaceBusy;
    }
    flipped_ = true;
    // The back buffer's pixels become the front's; the back buffer keeps them (the header).
    const HResult copied = copy(*back, back->bounds(), bounds(), std::nullopt, false);
    if (copied != kDdOk) {
        return copied;
    }
    return role_ == SurfaceRole::kPrimary ? owner_.PresentPrimary(*this) : kDdOk;
}

std::vector<Surface *> Surface::AttachedSurfaces() const {
    std::vector<Surface *> all;
    if (next_ != nullptr) {
        all.push_back(next_);
    }
    all.insert(all.end(), added_.begin(), added_.end());
    return all;
}

HResult Surface::GetAttachedSurface(const SurfaceCaps &caps, Surface **out) {
    if (out == nullptr || caps.caps == 0) {
        return kDdErrInvalidParams;
    }
    *out = nullptr;
    for (Surface *surface : AttachedSurfaces()) {
        if ((surface->desc_.caps.caps & caps.caps) == caps.caps) {
            surface->AddRef();
            *out = surface;
            return kDdOk;
        }
    }
    return kDdErrNotFound;
}

HResult Surface::AddAttachedSurface(Surface *surface) {
    if (surface == nullptr || surface == this) {
        return kDdErrInvalidParams;
    }
    if (std::find(added_.begin(), added_.end(), surface) != added_.end()) {
        return kDdErrSurfaceAlreadyAttached;
    }
    if (surface->role_ != SurfaceRole::kZBuffer || role_ == SurfaceRole::kZBuffer ||
        surface->Width() != Width() || surface->Height() != Height() || ZBuffer() != nullptr) {
        return kDdErrCannotAttachSurface;
    }
    surface->AddRef(); // released by DeleteAttachedSurface or this surface's destruction
    added_.push_back(surface);
    return kDdOk;
}

HResult Surface::DeleteAttachedSurface(std::uint32_t, Surface *surface) {
    if (surface == nullptr) {
        std::vector<Surface *> all;
        all.swap(added_);
        for (Surface *attached : all) {
            attached->Release();
        }
        return kDdOk;
    }
    const auto found = std::find(added_.begin(), added_.end(), surface);
    if (found == added_.end()) {
        return kDdErrCannotDetachSurface;
    }
    added_.erase(found);
    surface->Release();
    return kDdOk;
}

Surface *Surface::ZBuffer() const {
    for (Surface *surface : added_) {
        if (surface->role_ == SurfaceRole::kZBuffer) {
            return surface;
        }
    }
    return nullptr;
}

HResult Surface::GetDC(SurfaceDesc &out) {
    if (dcOut_) {
        return kDdErrDcAlreadyCreated;
    }
    if (locked_) {
        return kDdErrSurfaceBusy;
    }
    SyncToHost();
    dcOut_ = true;
    beginHostEdit(bounds());
    (void)GetSurfaceDesc(out);
    out.flags |= kDdsdLpSurface;
    out.surface = pixels_;
    return kDdOk;
}

HResult Surface::ReleaseDC() {
    if (!dcOut_) {
        return kDdErrNoDc;
    }
    dcOut_ = false;
    endHostEdit(bounds());
    return afterWrite();
}

HResult Surface::GetSurfaceDesc(SurfaceDesc &out) const {
    out = desc_;
    if (sourceKey_) {
        out.flags |= kDdsdCkSrcBlt;
        out.srcBlt = *sourceKey_;
    }
    if (destinationKey_) {
        out.flags |= kDdsdCkDestBlt;
        out.destBlt = *destinationKey_;
    }
    if (role_ == SurfaceRole::kPrimary && !chain_.empty()) {
        out.flags |= kDdsdBackBufferCount;
        out.backBufferCount = static_cast<std::uint32_t>(chain_.size());
    }
    if (role_ == SurfaceRole::kTexture && (desc_.caps.caps & kDdscapsMipMap) != 0) {
        out.flags |= kDdsdMipMapCount;
        std::uint32_t levels = 1;
        for (const Surface *level = next_; level != nullptr; level = level->next_) {
            ++levels;
        }
        out.mipMapCount = levels;
    }
    return kDdOk;
}

HResult Surface::GetPixelFormat(PixelFormatDesc &out) const {
    out = desc_.pixelFormat;
    return kDdOk;
}

HResult Surface::GetCaps(SurfaceCaps &out) const {
    out = desc_.caps;
    return kDdOk;
}

HResult Surface::SetPalette(Palette *palette) {
    if (format_ != PixelFormat::kIndexed8) {
        return kDdErrNotPalettized;
    }
    if (palette == palette_) {
        return kDdOk;
    }
    if (palette != nullptr) {
        palette->AddRef(); // the surface holds its palette until another replaces it
    }
    if (palette_ != nullptr) {
        palette_->Release();
    }
    palette_ = palette;
    applyTextureState();
    if (role_ == SurfaceRole::kPrimary) {
        (void)owner_.PresentPrimary(*this);
    }
    return kDdOk;
}

HResult Surface::GetPalette(Palette **out) {
    if (out == nullptr) {
        return kDdErrInvalidParams;
    }
    *out = palette_;
    if (palette_ == nullptr) {
        return kDdErrNoPaletteAttached;
    }
    palette_->AddRef();
    return kDdOk;
}

HResult Surface::SetColorKey(std::uint32_t flags, const ColorKey *key) {
    const std::uint32_t which = flags & (kDdckeySrcBlt | kDdckeyDestBlt);
    if (which == 0) {
        return (flags & kDdckeyOverlay) != 0 ? kDdErrUnsupported : kDdErrInvalidParams;
    }
    if (which == (kDdckeySrcBlt | kDdckeyDestBlt)) {
        return kDdErrInvalidParams;
    }
    std::optional<ColorKey> value;
    if (key != nullptr) {
        value = *key;
        if ((flags & kDdckeyColorSpace) == 0) {
            value->high = value->low;
        }
    }
    if (which == kDdckeySrcBlt) {
        sourceKey_ = value;
        applyTextureState();
    } else {
        destinationKey_ = value;
    }
    return kDdOk;
}

HResult Surface::GetColorKey(std::uint32_t flags, ColorKey &out) const {
    const std::optional<ColorKey> &key = (flags & kDdckeySrcBlt) != 0 ? sourceKey_
                                         : (flags & kDdckeyDestBlt) != 0
                                             ? destinationKey_
                                             : std::optional<ColorKey>{};
    if ((flags & (kDdckeySrcBlt | kDdckeyDestBlt)) == 0) {
        return (flags & kDdckeyOverlay) != 0 ? kDdErrUnsupported : kDdErrInvalidParams;
    }
    if (!key) {
        return kDdErrNoColorKey;
    }
    out = *key;
    return kDdOk;
}

HResult Surface::SetClipper(Clipper *clipper) {
    if (clipper != nullptr) {
        clipper->AddRef(); // the surface holds its clipper until another replaces it
    }
    if (clipper_ != nullptr) {
        clipper_->Release();
    }
    clipper_ = clipper;
    return kDdOk;
}

HResult Surface::GetClipper(Clipper **out) {
    if (out == nullptr) {
        return kDdErrInvalidParams;
    }
    *out = clipper_;
    if (clipper_ == nullptr) {
        return kDdErrNoClipperAttached;
    }
    clipper_->AddRef();
    return kDdOk;
}

platform::Result<platform::TextureId> Surface::ensureTexture() {
    const DisplayModeState &mode = owner_.Mode();
    if (role_ == SurfaceRole::kPrimary && mode.bitsPerPixel != 8 && desc_.width == mode.width &&
        desc_.height == mode.height && format_ == owner_.PrimaryFormat()) {
        platform::Result<platform::TextureId> display = owner_.DisplayTexture();
        if (display.Ok() && display.Value() != display_) {
            // A new back buffer starts black: the host copy is what the primary shows, and
            // whatever only the old back buffer held is gone with it.
            display_ = display.Value();
            hostCurrent_ = true;
            markHostWritten(bounds());
        }
        return display;
    }
    if (root_ != nullptr && level_ != 0) {
        return root_->ensureTexture();
    }
    if (!texture_.Valid()) {
        platform::TextureDesc desc;
        desc.width = desc_.width;
        desc.height = desc_.height;
        desc.format = format_;
        desc.mipLevels = 1;
        if (role_ == SurfaceRole::kTexture) {
            for (const Surface *level = next_; level != nullptr; level = level->next_) {
                ++desc.mipLevels;
            }
        }
        desc.renderTarget = format_ != PixelFormat::kIndexed8 &&
                            ((desc_.caps.caps & kDdscaps3dDevice) != 0 ||
                             role_ == SurfaceRole::kZBuffer || role_ == SurfaceRole::kBackBuffer);
        desc.renderScale = desc.renderTarget ? renderScale_ : 1;
        platform::Result<platform::TextureId> made = owner_.Device().CreateTexture(desc);
        if (!made.Ok()) {
            return made.Code();
        }
        texture_ = made.Value();
        renderTarget_ = desc.renderTarget;
        // The new texture's content is undefined: every level uploads its host copy.
        markHostWritten(bounds());
        for (Surface *member : chain_) {
            if (member->level_ != 0) {
                member->markHostWritten(member->bounds());
            }
        }
        applyTextureState();
    }
    return texture_;
}

void Surface::UseRenderScale(std::uint8_t scale) {
    if (scale == 0 || scale == renderScale_ || level_ != 0 || format_ == PixelFormat::kIndexed8) {
        return;
    }
    if (texture_.Valid()) {
        SyncToHost(); // a z-buffer has nothing to read: its host copy counts as current
        (void)owner_.Device().DestroyTexture(texture_);
        texture_ = {};
        hostCurrent_ = true; // ensureTexture() uploads it all into the new texture
        dropHostWrites();
    }
    renderScale_ = scale;
}

platform::Status Surface::flush() {
    if (!hasHostWrites()) {
        return {};
    }
    if (format_ == PixelFormat::kDepth16) {
        dropHostWrites(); // the Gpu takes no depth pixels; a z-buffer is cleared, never uploaded
        return {};
    }
    platform::Result<platform::TextureId> texture = ensureTexture();
    if (!texture.Ok()) {
        return texture.Code();
    }
    if (!changes_.Empty()) {
        if (!(dirty_ && contains(dirtyArea_, changes_.Area())) &&
            !flushChanges(texture.Value()).Ok()) {
            // Without the keyed blit the changed area goes up whole, as on any other surface.
            dirtyArea_ = dirty_ ? unite(dirtyArea_, changes_.Area()) : changes_.Area();
            dirty_ = true;
        }
        changes_.Clear();
        if (!dirty_) {
            return {};
        }
    }
    const std::uint8_t *first = pixels_ + static_cast<std::size_t>(dirtyArea_.top) * desc_.pitch +
                                static_cast<std::size_t>(dirtyArea_.left) * pixelBytes(format_);
    const platform::Status written =
        owner_.Device().WriteTexture(texture.Value(), level_, toRect(dirtyArea_), first,
                                     static_cast<std::uint32_t>(desc_.pitch));
    if (written.Ok()) {
        dirty_ = false;
        owner_.CountUpload();
    }
    return written;
}

platform::Result<platform::TextureId> Surface::GpuTexture() {
    platform::Result<platform::TextureId> texture = ensureTexture();
    if (!texture.Ok()) {
        return texture;
    }
    const platform::Status own = flush();
    if (!own.Ok()) {
        return own.Code();
    }
    if (role_ == SurfaceRole::kTexture) {
        for (Surface *level : chain_) {
            const platform::Status written = level->flush();
            if (!written.Ok()) {
                return written.Code();
            }
        }
    }
    return texture;
}

void Surface::MarkGpuWritten() {
    // Host writes made since the last flush (a Lock inside a scene) are uploaded first, over the
    // pixels the Gpu has drawn. DirectDraw applies writes in call order and the surface keeps no
    // per-write order, so one rule is used: a host write wins where it overlaps a draw. It is
    // right for the usual order (draw, then lock to overlay text) and loses nothing elsewhere;
    // dropping the dirt, as before, lost every write that was not flushed by a later Gpu use.
    if (hasHostWrites()) {
        (void)flush(); // on a refusal the Gpu has no copy of it to be stale against
    }
    dropHostWrites();
    hostCurrent_ = false;
    ++uniqueness_;
}

void Surface::SyncToHost() {
    if (hostCurrent_) {
        return;
    }
    if (format_ == PixelFormat::kDepth16) {
        hostCurrent_ = true;
        return; // depth cannot be read back; the host copy of a z-buffer is never current
    }
    platform::Result<platform::TextureId> texture = ensureTexture();
    if (!texture.Ok()) {
        return; // the host copy stays stale, to be read again on the next sync
    }
    const platform::Status read = owner_.Device().ReadTexture(
        texture.Value(), level_, toRect(bounds()), pixels_, static_cast<std::uint32_t>(desc_.pitch));
    if (read.Ok()) {
        hostCurrent_ = true;
    }
}

void Surface::markHostWritten(const RectL &area) {
    dirtyArea_ = dirty_ ? unite(dirtyArea_, area) : area;
    dirty_ = true;
    ++uniqueness_;
}

bool Surface::tracksChanges() const {
    return level_ == 0 && renderScale_ > 1 && isGpuTarget() && format_ != PixelFormat::kDepth16 &&
           hostCurrent_;
}

void Surface::beginHostEdit(const RectL &area) {
    // A host copy that could not be read back is uploaded whole at Unlock, as before.
    editKept_ = tracksChanges();
    if (!editKept_) {
        return;
    }
    if (!changes_.SizedFor(desc_.width, desc_.height)) {
        changes_.Reset(desc_.width, desc_.height); // first use: nothing is marked yet
    }
    const std::size_t rowBytes = static_cast<std::size_t>(area.right - area.left) * pixelBytes(format_);
    editBefore_.resize(rowBytes * static_cast<std::size_t>(area.bottom - area.top));
    for (std::int32_t y = area.top; y < area.bottom; ++y) {
        std::memcpy(editBefore_.data() + static_cast<std::size_t>(y - area.top) * rowBytes,
                    pixels_ + static_cast<std::size_t>(y) * desc_.pitch +
                        static_cast<std::size_t>(area.left) * pixelBytes(format_),
                    rowBytes);
    }
}

void Surface::endHostEdit(const RectL &area) {
    if (!editKept_ || !tracksChanges()) {
        editKept_ = false;
        markHostWritten(area);
        return;
    }
    editKept_ = false;
    const std::uint32_t bytes = pixelBytes(format_);
    const auto rowBytes = static_cast<std::uint32_t>(area.right - area.left) * bytes;
    changes_.Compare(area, editBefore_.data(), rowBytes,
                     pixels_ + static_cast<std::size_t>(area.top) * desc_.pitch +
                         static_cast<std::size_t>(area.left) * bytes,
                     static_cast<std::uint32_t>(desc_.pitch), bytes);
    ++uniqueness_;
}

RectL Surface::hostWriteArea() const {
    if (changes_.Empty()) {
        return dirtyArea_;
    }
    return dirty_ ? unite(dirtyArea_, changes_.Area()) : changes_.Area();
}

void Surface::dropHostWrites() {
    dirty_ = false;
    changes_.Clear();
}

platform::Status Surface::flushChanges(platform::TextureId texture) {
    platform::Gpu &gpu = owner_.Device();
    const RectL &area = changes_.Area();
    const std::uint32_t bytes = pixelBytes(format_);
    if (changes_.Whole()) {
        const platform::Status written = gpu.WriteTexture(
            texture, 0, toRect(area),
            pixels_ + static_cast<std::size_t>(area.top) * desc_.pitch +
                static_cast<std::size_t>(area.left) * bytes,
            static_cast<std::uint32_t>(desc_.pitch));
        if (written.Ok()) {
            owner_.CountUpload();
        }
        return written;
    }
    if (!patchTexture_.Valid()) {
        platform::TextureDesc desc;
        desc.width = desc_.width;
        desc.height = desc_.height;
        desc.format = format_;
        platform::Result<platform::TextureId> made = gpu.CreateTexture(desc);
        if (!made.Ok()) {
            return made.Code();
        }
        patchTexture_ = made.Value();
    }
    const std::uint32_t key = changes_.Patch(pixels_, static_cast<std::uint32_t>(desc_.pitch),
                                             bytes, formatMask(format_), patch_);
    const auto patchPitch = static_cast<std::uint32_t>(area.right - area.left) * bytes;
    platform::Status status =
        gpu.WriteTexture(patchTexture_, 0, toRect(area), patch_.data(), patchPitch);
    if (status.Ok()) {
        status = gpu.SetColorKey(patchTexture_, key);
    }
    if (status.Ok()) {
        platform::BlitDesc blit;
        blit.source = patchTexture_;
        blit.sourceRect = toRect(area);
        blit.destination = texture;
        blit.destinationRect = toRect(area);
        blit.sourceColorKey = true;
        status = gpu.Blit(blit);
    }
    if (status.Ok()) {
        owner_.CountUpload();
    }
    return status;
}

HResult Surface::afterWrite() {
    // Nothing is uploaded here: the changed area waits in dirtyArea_ for the next flush, which
    // comes when the Gpu next uses the texture (the header). A primary shows what is written to
    // it, until the game starts flipping a chain: after the first Flip, Flip presents and direct
    // writes to the front do not.
    if (role_ == SurfaceRole::kPrimary && !(isFlipFront() && flipped_)) {
        (void)owner_.PresentPrimary(*this);
    }
    return kDdOk;
}

HResult Surface::fill(const RectL &area, std::uint32_t value) {
    // The Gpu fills a Direct3D target (the clear before a scene then costs no upload), a surface
    // it drew into since, and a z-buffer, whose pixels it never takes from the host; a current
    // host copy is filled too. Any other surface takes the fill in its host copy, uploaded with
    // the rest of the frame's changes at the next flush.
    if (level_ == 0 && (!hostCurrent_ || format_ == PixelFormat::kDepth16 || isGpuTarget())) {
        const bool hostWasCurrent = hostCurrent_;
        // A current host copy gets the same fill, so what it has not uploaded yet needs no
        // flush first: the next flush uploads it, fill included, and drops it if the fill
        // covered it all.
        platform::Result<platform::TextureId> texture =
            hostWasCurrent ? ensureTexture() : GpuTexture();
        if (texture.Ok() && owner_.Device().Fill(texture.Value(), toRect(area), value).Ok()) {
            if (hostWasCurrent) {
                fillOnHost(area, value); // keeps the host copy current for the next Lock
                if (hasHostWrites() && contains(area, hostWriteArea())) {
                    dropHostWrites();
                }
                ++uniqueness_;
            } else {
                MarkGpuWritten();
            }
            return afterWrite();
        }
    }
    SyncToHost();
    fillOnHost(area, value);
    markHostWritten(area);
    return afterWrite();
}

HResult Surface::copy(Surface &source, const RectL &sourceArea, const RectL &area,
                      std::optional<std::uint32_t> key, bool keyOverride) {
    const bool converts = source.format_ == PixelFormat::kIndexed8 &&
                          format_ != PixelFormat::kIndexed8 && format_ != PixelFormat::kDepth16;
    // Two current host copies of one format copy on the host: the 2D screens lock, blit and lock
    // again many times a frame, and a Gpu blit there would cost an upload of the destination
    // before it and a read back at the next Lock. A Direct3D target as the source (the back
    // buffer at Flip) copies on the Gpu: its texture is flushed once and the frame is not
    // uploaded a second time through the destination.
    const bool onHost = hostCurrent_ && source.hostCurrent_ && source.format_ == format_ &&
                        format_ != PixelFormat::kDepth16 && !source.isGpuTarget();
    if (!onHost && &source != this && level_ == 0 && source.level_ == 0 &&
        format_ != PixelFormat::kDepth16 && (source.format_ == format_ || converts)) {
        platform::Result<platform::TextureId> from = source.GpuTexture();
        // A blit without a key replaces every pixel of `area`: what the destination has not
        // uploaded there is not uploaded first (the game writes the front between Flips, and
        // the next Flip covers it all).
        const bool replacesDirty = !key && hasHostWrites() && contains(area, hostWriteArea());
        platform::Result<platform::TextureId> to = replacesDirty ? ensureTexture() : GpuTexture();
        if (from.Ok() && to.Ok()) {
            platform::Gpu &gpu = owner_.Device();
            if (keyOverride) {
                (void)gpu.SetColorKey(from.Value(), key);
            }
            platform::BlitDesc blit;
            blit.source = from.Value();
            blit.sourceRect = toRect(sourceArea);
            blit.destination = to.Value();
            blit.destinationRect = toRect(area);
            blit.sourceColorKey = key.has_value();
            const platform::Status copied = gpu.Blit(blit);
            if (keyOverride) {
                (void)gpu.SetColorKey(from.Value(), source.gpuSourceKey());
            }
            if (copied.Ok()) {
                if (replacesDirty) {
                    dropHostWrites(); // the blit covered it: nothing to upload under the result
                }
                MarkGpuWritten();
                return afterWrite();
            }
        }
    }
    if (source.format_ != format_) {
        return kDdErrInvalidPixelFormat;
    }
    source.SyncToHost();
    SyncToHost();
    copyOnHost(source, sourceArea, area, key);
    markHostWritten(area);
    return afterWrite();
}

void Surface::copyOnHost(Surface &source, const RectL &sourceArea, const RectL &area,
                         std::optional<std::uint32_t> key) {
    const std::uint32_t bytes = pixelBytes(format_);
    const std::uint32_t sw = static_cast<std::uint32_t>(sourceArea.right - sourceArea.left);
    const std::uint32_t sh = static_cast<std::uint32_t>(sourceArea.bottom - sourceArea.top);
    const std::uint32_t dw = static_cast<std::uint32_t>(area.right - area.left);
    const std::uint32_t dh = static_cast<std::uint32_t>(area.bottom - area.top);
    // Read the source first, so that a copy within one surface may overlap.
    std::vector<std::uint8_t> copied(static_cast<std::size_t>(sw) * sh * bytes);
    for (std::uint32_t y = 0; y < sh; ++y) {
        std::memcpy(copied.data() + static_cast<std::size_t>(y) * sw * bytes,
                    source.pixels_ +
                        static_cast<std::size_t>(sourceArea.top + y) * source.desc_.pitch +
                        static_cast<std::size_t>(sourceArea.left) * bytes,
                    static_cast<std::size_t>(sw) * bytes);
    }
    for (std::uint32_t y = 0; y < dh; ++y) {
        const std::uint32_t sy =
            static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) * sh / dh);
        std::uint8_t *row = pixels_ + static_cast<std::size_t>(area.top + y) * desc_.pitch +
                            static_cast<std::size_t>(area.left) * bytes;
        for (std::uint32_t x = 0; x < dw; ++x) {
            const std::uint32_t sx =
                static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) * sw / dw);
            const std::uint32_t pixel =
                readPixel(copied.data() + (static_cast<std::size_t>(sy) * sw + sx) * bytes, bytes);
            if (!key || pixel != *key) {
                writePixel(row + static_cast<std::size_t>(x) * bytes, bytes, pixel);
            }
        }
    }
}

void Surface::fillOnHost(const RectL &area, std::uint32_t value) {
    const std::uint32_t bytes = pixelBytes(format_);
    for (std::int32_t y = area.top; y < area.bottom; ++y) {
        std::uint8_t *row = pixels_ + static_cast<std::size_t>(y) * desc_.pitch;
        for (std::int32_t x = area.left; x < area.right; ++x) {
            writePixel(row + static_cast<std::size_t>(x) * bytes, bytes, value);
        }
    }
}

std::optional<std::uint32_t> Surface::gpuSourceKey() const {
    if (!sourceKey_) {
        return std::nullopt;
    }
    return sourceKey_->low & formatMask(format_);
}

void Surface::applyTextureState() {
    if (!texture_.Valid()) {
        return;
    }
    platform::Gpu &gpu = owner_.Device();
    if (format_ != PixelFormat::kDepth16) {
        (void)gpu.SetColorKey(texture_, gpuSourceKey());
    }
    if (format_ == PixelFormat::kIndexed8) {
        platform::PaletteId palette;
        if (palette_ != nullptr) {
            platform::Result<platform::PaletteId> made = palette_->GpuPalette();
            if (made.Ok()) {
                palette = made.Value();
            }
        }
        (void)gpu.AttachPalette(texture_, palette);
    }
}

} // namespace sfcop::hle::ddraw4
