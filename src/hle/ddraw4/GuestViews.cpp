#include "hle/ddraw4/GuestViews.h"

#include <vector>

namespace sfcop::hle::ddraw4 {

namespace {

struct Host {
    platform::Gpu *gpu = nullptr;
    platform::Window *window = nullptr;
};

Host &host() {
    static Host state;
    return state;
}

} // namespace

GuestViews &GuestViews::Instance() {
    static GuestViews views;
    return views;
}

void GuestViews::SetMaker(GuestInterface first, GuestInterface last, MakeView make) {
    for (auto i = static_cast<std::uint32_t>(first); i <= static_cast<std::uint32_t>(last); ++i) {
        makers_[static_cast<GuestInterface>(i)] = make;
    }
}

std::optional<GuestInterface> GuestViews::OwnInterface(GuestInterface iface, const Guid &iid) {
    using GI = GuestInterface;
    switch (iface) {
    case GI::kDirectDraw:
    case GI::kDirectDraw2:
    case GI::kDirectDraw4:
        if (iid == kIidUnknown || iid == kIidDirectDraw) {
            return GI::kDirectDraw;
        }
        if (iid == kIidDirectDraw2) {
            return GI::kDirectDraw2;
        }
        if (iid == kIidDirectDraw4) {
            return GI::kDirectDraw4;
        }
        break;
    case GI::kSurface3:
    case GI::kSurface4:
        if (iid == kIidUnknown || iid == kIidDirectDrawSurface || iid == kIidDirectDrawSurface2 ||
            iid == kIidDirectDrawSurface3) {
            return GI::kSurface3;
        }
        if (iid == kIidDirectDrawSurface4) {
            return GI::kSurface4;
        }
        break;
    case GI::kPalette:
        if (iid == kIidUnknown || iid == kIidDirectDrawPalette) {
            return GI::kPalette;
        }
        break;
    case GI::kClipper:
        if (iid == kIidUnknown || iid == kIidDirectDrawClipper) {
            return GI::kClipper;
        }
        break;
    default:
        break;
    }
    return std::nullopt;
}

std::uint32_t GuestViews::Query(ComObject &host, GuestInterface iface, const Guid &iid) {
    if (const auto own = OwnInterface(iface, iid)) {
        const std::uint32_t view = View(&host, *own);
        if (view != 0) {
            host.AddRef();
        }
        return view;
    }
    return queryHook_ != nullptr ? queryHook_(host, iface, iid) : 0;
}

std::uint32_t GuestViews::View(ComObject *host, GuestInterface iface) {
    if (host == nullptr) {
        return 0;
    }
    const auto found = byHost_.find({host, iface});
    if (found != byHost_.end()) {
        return found->second;
    }
    const auto maker = makers_.find(iface);
    if (maker == makers_.end()) {
        return 0;
    }
    const std::uint32_t address = maker->second(iface);
    if (address == 0) {
        return 0;
    }
    byHost_[{host, iface}] = address;
    byAddress_[address] = GuestView{host, iface};
    return address;
}

std::optional<GuestView> GuestViews::Find(std::uint32_t address) const {
    const auto found = byAddress_.find(address);
    if (found == byAddress_.end()) {
        return std::nullopt;
    }
    return found->second;
}

ComObject *GuestViews::Host(std::uint32_t address, GuestInterface first,
                            GuestInterface last) const {
    const auto view = Find(address);
    if (!view || view->iface < first || view->iface > last) {
        return nullptr;
    }
    return view->host;
}

std::vector<ComObject *> GuestViews::Hosts(GuestInterface iface) const {
    std::vector<ComObject *> hosts;
    for (const auto &[address, view] : byAddress_) {
        if (view.iface == iface) {
            hosts.push_back(view.host);
        }
    }
    return hosts;
}

void GuestViews::HostDestroyed(const ComObject &host) {
    std::vector<std::uint32_t> freed;
    for (auto i = byHost_.lower_bound({&host, GuestInterface{}});
         i != byHost_.end() && i->first.first == &host;) {
        freed.push_back(i->second);
        byAddress_.erase(i->second);
        i = byHost_.erase(i);
    }
    if (free_ != nullptr) {
        for (const std::uint32_t address : freed) {
            free_(address);
        }
    }
}

void GuestViews::Reset() {
    byAddress_.clear();
    byHost_.clear();
}

void SetGuestHost(platform::Gpu *gpu, platform::Window *window) {
    host() = Host{gpu, window};
}

platform::Gpu *GuestGpu() {
    return host().gpu;
}

platform::Window *GuestWindow() {
    return host().window;
}

} // namespace sfcop::hle::ddraw4
