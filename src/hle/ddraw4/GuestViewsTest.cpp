#include "hle/ddraw4/GuestViews.h"

#include <doctest/doctest.h>

#include <vector>

namespace sfcop::hle::ddraw4 {

namespace {

class Thing final : public ComObject {};

std::vector<std::uint32_t> g_made;
std::vector<std::uint32_t> g_freed;
std::uint32_t g_next = 0x1000;

std::uint32_t makeView(GuestInterface) {
    g_made.push_back(g_next);
    g_next += 0x10;
    return g_made.back();
}

std::uint32_t noMemory(GuestInterface) {
    return 0;
}

void freeView(std::uint32_t address) {
    g_freed.push_back(address);
}

// A fresh registry state for each test, with the test's own maker and freer.
struct Fixture {
    GuestViews &views = GuestViews::Instance();
    Fixture() {
        views.Reset();
        g_made.clear();
        g_freed.clear();
        g_next = 0x1000;
        views.SetMaker(GuestInterface::kDirectDraw, GuestInterface::kTexture2, makeView);
        views.SetFreer(freeView);
    }
    ~Fixture() {
        views.Reset();
        views.SetFreer(nullptr);
    }
};

} // namespace

TEST_CASE("a host object gets one view per interface, made on first use") {
    Fixture fixture;
    Thing *thing = new Thing;
    const std::uint32_t four = fixture.views.View(thing, GuestInterface::kDirectDraw4);
    CHECK(four == 0x1000u);
    CHECK(fixture.views.View(thing, GuestInterface::kDirectDraw4) == 0x1000u);
    CHECK(fixture.views.View(thing, GuestInterface::kDirectDraw) == 0x1010u);
    CHECK(g_made == std::vector<std::uint32_t>{0x1000, 0x1010});
    const auto view = fixture.views.Find(0x1010);
    REQUIRE(view.has_value());
    CHECK(view->host == thing);
    CHECK(view->iface == GuestInterface::kDirectDraw);
    CHECK_FALSE(fixture.views.Find(0x1020).has_value());
    CHECK(fixture.views.View(nullptr, GuestInterface::kDirectDraw) == 0u);
    thing->Release();
}

TEST_CASE("Host finds an object only through the interfaces asked for") {
    Fixture fixture;
    Thing *thing = new Thing;
    const std::uint32_t surface = fixture.views.View(thing, GuestInterface::kSurface4);
    CHECK(fixture.views.Host(surface, GuestInterface::kSurface3, GuestInterface::kSurface4) ==
          thing);
    CHECK(fixture.views.Host(surface, GuestInterface::kDirectDraw, GuestInterface::kDirectDraw4) ==
          nullptr);
    CHECK(fixture.views.Host(0, GuestInterface::kDirectDraw, GuestInterface::kTexture2) == nullptr);
    thing->Release();
}

TEST_CASE("Hosts lists the objects shown as one interface") {
    Fixture fixture;
    Thing *first = new Thing;
    Thing *second = new Thing;
    fixture.views.View(first, GuestInterface::kTexture2);
    fixture.views.View(second, GuestInterface::kTexture2);
    fixture.views.View(second, GuestInterface::kSurface4);
    CHECK(fixture.views.Hosts(GuestInterface::kTexture2) ==
          std::vector<ComObject *>{first, second});
    CHECK(fixture.views.Hosts(GuestInterface::kSurface4) == std::vector<ComObject *>{second});
    CHECK(fixture.views.Hosts(GuestInterface::kClipper).empty());
    first->Release();
    second->Release();
}

TEST_CASE("a destroyed host object's views are forgotten and freed") {
    Fixture fixture;
    Thing *kept = new Thing;
    Thing *gone = new Thing;
    const std::uint32_t keptView = fixture.views.View(kept, GuestInterface::kPalette);
    fixture.views.View(gone, GuestInterface::kSurface3);
    fixture.views.View(gone, GuestInterface::kSurface4);
    ComObject::SetDestroyHook(
        [](const ComObject *object) { GuestViews::Instance().HostDestroyed(*object); });
    gone->Release();
    ComObject::SetDestroyHook(nullptr);
    CHECK(g_freed == std::vector<std::uint32_t>{0x1010, 0x1020});
    CHECK(fixture.views.Size() == 1);
    CHECK_FALSE(fixture.views.Find(0x1010).has_value());
    CHECK(fixture.views.Find(keptView)->host == kept);
    kept->Release();
}

TEST_CASE("no view is made without a maker or without guest memory") {
    Fixture fixture;
    Thing *thing = new Thing;
    fixture.views.SetMaker(GuestInterface::kClipper, GuestInterface::kClipper, noMemory);
    CHECK(fixture.views.View(thing, GuestInterface::kClipper) == 0u);
    CHECK(fixture.views.Size() == 0);
    thing->Release();
}

TEST_CASE("the guest host is what the runtime set") {
    CHECK(GuestGpu() == nullptr);
    CHECK(GuestWindow() == nullptr);
    auto *gpu = reinterpret_cast<platform::Gpu *>(0x10); // compared, never used
    auto *window = reinterpret_cast<platform::Window *>(0x20);
    SetGuestHost(gpu, window);
    CHECK(GuestGpu() == gpu);
    CHECK(GuestWindow() == window);
    SetGuestHost(nullptr, nullptr);
}

} // namespace sfcop::hle::ddraw4
