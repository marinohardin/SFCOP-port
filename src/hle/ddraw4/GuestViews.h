// What the DirectDraw and Direct3D guest shims (Ddraw4Guest.cpp, D3d3Guest.cpp) share: the game's
// COM objects for the host objects of these modules, and the Gpu and Window they draw with.
//
// The game sees a COM object as a guest address: a small block in guest memory whose first word
// is its interface's vtable. The shims make one such view per host object and interface, on
// first use, and find the host object again from the address the game calls with. All interfaces
// of a host object share its one reference count (ComObject.h), so a view holds no count of its
// own; it is freed when its host object is destroyed.
//
// The shims are free functions the guest calls with no context, so this state is process-wide.
// It is plain C++ (no guest memory access): the shims supply the functions that make and free
// the guest blocks.
#pragma once

#include "hle/ddraw4/ComObject.h"
#include "hle/ddraw4/Ddraw4Defs.h"

#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace sfcop::platform {
class Gpu;
class Window;
} // namespace sfcop::platform

namespace sfcop::hle::ddraw4 {

// The interfaces a view can show.
enum class GuestInterface : std::uint32_t {
    kDirectDraw = 1,
    kDirectDraw2,
    kDirectDraw4,
    kSurface3,
    kSurface4,
    kPalette,
    kClipper,
    kDirect3D3,
    kDevice3,
    kViewport3,
    kTexture2,
};

struct GuestView {
    ComObject *host = nullptr;
    GuestInterface iface = GuestInterface::kDirectDraw;
};

class GuestViews {
public:
    // Makes the guest block for a view of `host` and returns its address (0 if guest memory is
    // exhausted). Frees one.
    using MakeView = std::uint32_t (*)(GuestInterface iface);
    using FreeView = void (*)(std::uint32_t address);

    // The Direct3D module's answer to a QueryInterface the DirectDraw objects do not serve
    // themselves (IDirect3D3 from a DirectDraw object, IDirect3DTexture2 from a surface): the
    // address of a view holding a new reference, or 0 for E_NOINTERFACE.
    using QueryHook = std::uint32_t (*)(ComObject &host, GuestInterface hostInterface,
                                        const Guid &iid);

    static GuestViews &Instance();

    // `make` serves the interfaces from `first` to `last`.
    void SetMaker(GuestInterface first, GuestInterface last, MakeView make);
    void SetFreer(FreeView free) { free_ = free; }
    void SetQueryHook(QueryHook hook) { queryHook_ = hook; }
    QueryHook Hook() const { return queryHook_; }

    // QueryInterface's decision, shared by every interface's QueryInterface shim: the address of
    // the view of `host` (shown as `iface`) that `iid` names, holding one new reference on the
    // object that view shows, or 0 for E_NOINTERFACE, in which case no count has changed. The
    // DirectDraw interfaces are answered here, from OwnInterface; the Direct3D ones by the hook.
    std::uint32_t Query(ComObject &host, GuestInterface iface, const Guid &iid);

    // The DirectDraw interface `iid` names on an object shown as `iface`, if that object serves it
    // itself. IDirectDraw and IUnknown both give kDirectDraw; IDirectDrawSurface and
    // IDirectDrawSurface2 are prefixes of IDirectDrawSurface3's vtable, so they give kSurface3.
    static std::optional<GuestInterface> OwnInterface(GuestInterface iface, const Guid &iid);

    // The address of `host`'s view as `iface`, made on first use; 0 for a null host, an
    // interface with no maker, or exhausted guest memory.
    std::uint32_t View(ComObject *host, GuestInterface iface);

    // The view at `address`, or std::nullopt for an address that is not one.
    std::optional<GuestView> Find(std::uint32_t address) const;

    // The host object at `address` if it shows one of the interfaces `first` to `last`.
    ComObject *Host(std::uint32_t address, GuestInterface first, GuestInterface last) const;

    // Every live host object with a view as `iface`, in no particular order.
    std::vector<ComObject *> Hosts(GuestInterface iface) const;

    // Forgets and frees every view of a host object being destroyed (ComObject's destroy hook).
    void HostDestroyed(const ComObject &host);

    std::size_t Size() const { return byAddress_.size(); }

    // Forgets every view without freeing it (tests).
    void Reset();

private:
    std::map<std::uint32_t, GuestView> byAddress_;
    std::map<std::pair<const ComObject *, GuestInterface>, std::uint32_t> byHost_;
    std::map<GuestInterface, MakeView> makers_;
    FreeView free_ = nullptr;
    QueryHook queryHook_ = nullptr;
};

// The Gpu (attached to the Window) that DirectDrawCreate gives the DirectDraw objects. The guest
// runtime sets both before the game creates its first DirectDraw object; while they are unset
// the shims hand DirectDrawCreate to the boot platform's DirectDraw (TEMPORARY, until the guest
// runtime replaces the boot platform). Both outlive every DirectDraw object.
void SetGuestHost(platform::Gpu *gpu, platform::Window *window);
platform::Gpu *GuestGpu();
platform::Window *GuestWindow();

} // namespace sfcop::hle::ddraw4
