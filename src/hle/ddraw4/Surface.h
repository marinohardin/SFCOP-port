// The surface behind IDirectDrawSurface3 and IDirectDrawSurface4: one object, two views
// (ComObject.h).
//
// A surface keeps its pixels twice: in host memory the game can lock (PixelMemory), and in a Gpu
// texture that blits, Direct3D and the display use. Each copy knows whether it is current. Lock
// and GetDC bring the host copy up to date (a Gpu read) and mark the locked area changed. Nothing
// is uploaded at Unlock or ReleaseDC: the changed areas merge into one rectangle, uploaded once at
// the surface's next Gpu use (a bind, a draw, a Gpu blit, Flip). Invariant: a texture-backed
// surface's Gpu copy is stale between a host write and the next flush, so every Gpu use goes
// through GpuTexture(), which flushes. A Gpu fill or unkeyed Gpu blit that covers the whole
// changed area drops it instead. Blt and BltFast run on the Gpu when both surfaces have a texture
// and on the host copy otherwise (two current host copies of one format, the same surface on both
// sides, a mipmap level, a texture the Gpu refused); a Direct3D target is filled on the Gpu and is
// the Gpu side of a copy from it. Textures are made on first use, so a surface the game only
// locks costs no Gpu memory.
//
// The primary surface's texture is the display's back buffer at 16 and 32 bits (DirectDraw.h).
// Flip copies the first back buffer to the primary and presents; the back buffer keeps its
// content. A primary without a back buffer is presented after every change; one with a flipping
// chain at Flip only.
#pragma once

#include "hle/ddraw4/ComObject.h"
#include "hle/ddraw4/Ddraw4Defs.h"

#include <vector>

namespace sfcop::hle::ddraw4 {

class Clipper;
class DirectDraw;
class Palette;

enum class SurfaceRole : std::uint8_t {
    kPrimary,    // the front buffer: what the display shows
    kBackBuffer, // a back buffer of a flipping chain
    kPlain,      // an offscreen surface
    kTexture,    // a texture, or one level of a mipmapped texture
    kZBuffer,    // a 16-bit depth buffer
};

class Surface final : public ComObject {
public:
    // Use DirectDraw::CreateSurface(). `root` is the surface whose chain this one belongs to (the
    // primary for a back buffer, the top level for a mipmap level), or nullptr. A surface with a
    // root starts with no reference of the game's and lives as long as its root.
    Surface(DirectDraw &owner, SurfaceRole role, const SurfaceDesc &desc,
            platform::PixelFormat format, Surface *root, std::uint8_t level);

    // Whether the constructor got its host memory.
    bool Valid() const { return pixels_ != nullptr; }

    // Locks the whole surface or `rect` and describes it in `out`, with `out.surface` the first
    // locked pixel. DDERR_SURFACEBUSY while locked or while a DC is out; DDERR_INVALIDRECT for a
    // rectangle outside the surface.
    HResult Lock(const RectL *rect, SurfaceDesc &out, std::uint32_t flags);

    // Marks what the lock may have changed, for the next flush. DDERR_NOTLOCKED if not locked.
    HResult Unlock();

    // DDBLT_COLORFILL, the BLACKNESS, WHITENESS and SRCCOPY raster operations, stretching, and the
    // source colour key (the surface's or DDBLTFX's). Destination keys, other raster operations
    // and DDBLTFX effects are DDERR_UNSUPPORTED. With a clipper attached the destination is
    // clipped to the surface; without one a rectangle outside it is DDERR_INVALIDRECT.
    HResult Blt(const RectL *destination, Surface *source, const RectL *sourceRect,
                std::uint32_t flags, const BltFx *fx);

    // An unstretched copy to (x, y), with the source's colour key if DDBLTFAST_SRCCOLORKEY.
    HResult BltFast(std::uint32_t x, std::uint32_t y, Surface *source, const RectL *sourceRect,
                    std::uint32_t transfer);

    // Shows the first back buffer (`target` must be null or a back buffer of this chain).
    // DDERR_NOTFLIPPABLE unless this is the front of a flipping chain.
    HResult Flip(Surface *target, std::uint32_t flags);

    // The first surface attached to this one whose caps include all of `caps.caps`, AddRef'd:
    // the back buffer, the z-buffer, the next mipmap level. DDERR_NOTFOUND if none.
    HResult GetAttachedSurface(const SurfaceCaps &caps, Surface **out);

    // Attaches a z-buffer of the same size, AddRef'ing it. DDERR_CANNOTATTACHSURFACE for
    // anything else, DDERR_SURFACEALREADYATTACHED if it is.
    HResult AddAttachedSurface(Surface *surface);

    // Detaches and releases a surface attached with AddAttachedSurface (`surface` null: all of
    // them). DDERR_CANNOTDETACHSURFACE for one that is not.
    HResult DeleteAttachedSurface(std::uint32_t flags, Surface *surface);

    // The surfaces attached to this one, in order (for EnumAttachedSurfaces).
    std::vector<Surface *> AttachedSurfaces() const;

    // Makes the host copy current for a GDI drawing context and describes it as Lock does.
    // DDERR_DCALREADYCREATED if a DC is out, DDERR_SURFACEBUSY while locked.
    HResult GetDC(SurfaceDesc &out);

    // Marks what GDI drew, for the next flush. DDERR_NODC if no DC is out.
    HResult ReleaseDC();

    HResult GetSurfaceDesc(SurfaceDesc &out) const;
    HResult GetPixelFormat(PixelFormatDesc &out) const;
    HResult GetCaps(SurfaceCaps &out) const;

    // Attaches a palette (AddRef'd; null detaches) to an 8-bit surface. DDERR_NOTPALETTIZED for
    // others.
    HResult SetPalette(Palette *palette);
    HResult GetPalette(Palette **out);

    // DDCKEY_SRCBLT and DDCKEY_DESTBLT; a null key removes it. A colour space (a range) keeps only
    // its low value as the Gpu key. Overlay keys are DDERR_UNSUPPORTED.
    HResult SetColorKey(std::uint32_t flags, const ColorKey *key);
    HResult GetColorKey(std::uint32_t flags, ColorKey &out) const;

    HResult SetClipper(Clipper *clipper);
    HResult GetClipper(Clipper **out);

    // Nothing is ever lost: the Gpu keeps every texture.
    HResult IsLost() const { return kDdOk; }
    HResult Restore() { return kDdOk; }

    // Changes whenever the surface's content changes, as DirectDraw 4 documents.
    std::uint32_t UniquenessValue() const { return uniqueness_; }
    void ChangeUniquenessValue() { ++uniqueness_; }

    // What DirectDraw and Direct3D use.
    DirectDraw &Owner() const { return owner_; }
    SurfaceRole Role() const { return role_; }
    platform::PixelFormat Format() const { return format_; }
    std::uint32_t Width() const { return desc_.width; }
    std::uint32_t Height() const { return desc_.height; }
    std::uint8_t Level() const { return level_; }
    Palette *AttachedPalette() const { return palette_; }

    // The attached z-buffer, or nullptr.
    Surface *ZBuffer() const;

    // The surface's texture with the host copy's changes uploaded (the one place they are). The
    // texture is made on the first call; a mipmap level answers with its root's texture.
    platform::Result<platform::TextureId> GpuTexture();

    // Records that the Gpu drew into the texture, so the host copy is stale.
    void MarkGpuWritten();

    // Brings the host copy up to date from the Gpu. If the read fails the host copy stays
    // stale and the next sync tries again.
    void SyncToHost();

    // The Direct3D 3 texture handle naming this surface (0 = none yet), and the hook called with
    // it when the surface is destroyed (the Direct3D module removes the handle there).
    std::uint32_t TextureHandle() const { return textureHandle_; }
    void SetTextureHandle(std::uint32_t handle) { textureHandle_ = handle; }
    using HandleGoneHook = void (*)(std::uint32_t handle);
    static void SetHandleGoneHook(HandleGoneHook hook);

private:
    friend class DirectDraw; // builds flipping chains and mipmap levels

    ~Surface() override;
    bool KeepAlive() const override { return root_ != nullptr; }

    RectL bounds() const;
    bool isFlipFront() const;
    // Whether the surface has its own texture and Direct3D can draw into it.
    bool isGpuTarget() const { return texture_.Valid() && renderTarget_; }
    platform::Result<platform::TextureId> ensureTexture();
    platform::Status flush();
    void markHostWritten(const RectL &area);
    HResult afterWrite();
    HResult fill(const RectL &area, std::uint32_t value);
    HResult copy(Surface &source, const RectL &sourceArea, const RectL &area,
                 std::optional<std::uint32_t> key, bool keyOverride);
    void copyOnHost(Surface &source, const RectL &sourceArea, const RectL &area,
                    std::optional<std::uint32_t> key);
    void fillOnHost(const RectL &area, std::uint32_t value);
    void applyTextureState();
    std::optional<std::uint32_t> gpuSourceKey() const;

    DirectDraw &owner_;
    SurfaceRole role_;
    SurfaceDesc desc_;
    platform::PixelFormat format_;
    Surface *root_;      // the chain's root; it destroys this surface
    std::uint8_t level_; // the mipmap level, 0 for every other surface
    std::uint8_t *pixels_ = nullptr;
    std::vector<Surface *> chain_; // back buffers or mipmap levels this surface destroys
    Surface *next_ = nullptr;      // the next surface of the chain: a back buffer, a level
    std::vector<Surface *> added_; // AddAttachedSurface's, each holding one reference
    platform::TextureId texture_;  // own texture (never the display's), zero until made
    platform::TextureId display_;  // the display texture the primary last drew into
    bool renderTarget_ = false;    // texture_ was made as a render target
    bool hostCurrent_ = true;      // the host copy has the latest pixels
    bool dirty_ = false;           // the host copy has pixels the Gpu has not seen
    RectL dirtyArea_;
    bool locked_ = false;
    bool lockReadOnly_ = false;
    RectL lockArea_;
    bool dcOut_ = false;
    bool flipped_ = false;               // Flip has been called: the game flips this chain
    std::uint32_t textureHandle_ = 0;
    std::optional<ColorKey> sourceKey_;
    std::optional<ColorKey> destinationKey_;
    Palette *palette_ = nullptr;
    Clipper *clipper_ = nullptr;
    std::uint32_t uniqueness_ = 1;
};

} // namespace sfcop::hle::ddraw4
