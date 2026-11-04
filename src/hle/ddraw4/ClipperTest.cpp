#include "hle/ddraw4/Clipper.h"
#include "hle/ddraw4/Ddraw4TestRig.h"

namespace sfcop::hle::ddraw4 {

using test::Rig;

TEST_CASE("a clipper remembers its window") {
    Rig rig;
    Clipper *clipper = nullptr;
    REQUIRE(rig.dd->CreateClipper(0, &clipper) == kDdOk);
    std::uint32_t window = 1;
    CHECK(clipper->GetHWnd(window) == kDdOk);
    CHECK(window == 0);
    CHECK(clipper->SetHWnd(0, 0x00010001) == kDdOk);
    CHECK(clipper->GetHWnd(window) == kDdOk);
    CHECK(window == 0x00010001);
    CHECK(clipper->SetHWnd(1, 0x00010001) == kDdErrInvalidParams);
    clipper->Release();
}

TEST_CASE("a clipper's list is the display with a window, or the list set") {
    Rig rig;
    REQUIRE(rig.dd->SetDisplayMode(800, 600, 16, 0, 0) == kDdOk);
    Clipper *clipper = nullptr;
    REQUIRE(rig.dd->CreateClipper(0, &clipper) == kDdOk);
    std::vector<RectL> rects;
    CHECK(clipper->GetClipList(nullptr, rects) == kDdErrNoClipList);

    REQUIRE(clipper->SetHWnd(0, 0x00010001) == kDdOk);
    CHECK(clipper->GetClipList(nullptr, rects) == kDdOk);
    REQUIRE(rects.size() == 1);
    CHECK(rects[0].right == 800);
    CHECK(rects[0].bottom == 600);
    const RectL clip{700, 500, 900, 700};
    CHECK(clipper->GetClipList(&clip, rects) == kDdOk);
    REQUIRE(rects.size() == 1);
    CHECK(rects[0].left == 700);
    CHECK(rects[0].right == 800);

    const std::vector<RectL> list{{0, 0, 10, 10}, {20, 20, 30, 30}};
    CHECK(clipper->SetClipList(&list, 0) == kDdOk);
    CHECK(clipper->GetClipList(nullptr, rects) == kDdOk);
    CHECK(rects.size() == 2);
    const RectL corner{0, 0, 15, 15};
    CHECK(clipper->GetClipList(&corner, rects) == kDdOk);
    REQUIRE(rects.size() == 1);
    CHECK(rects[0].right == 10);
    CHECK(clipper->SetClipList(nullptr, 0) == kDdOk);
    CHECK(clipper->GetClipList(nullptr, rects) == kDdOk);
    CHECK(rects.size() == 1); // back to the window's

    bool changed = true;
    CHECK(clipper->IsClipListChanged(changed) == kDdOk);
    CHECK_FALSE(changed);
    CHECK(clipper->Initialize() == kDdErrAlreadyInitialized);
    clipper->Release();
}

TEST_CASE("a clipper made without a DirectDraw object lives by its own count") {
    test::destroyed().clear();
    ComObject::SetDestroyHook(&test::recordDestroyed);
    Clipper *clipper = new Clipper(nullptr); // as DirectDrawCreateClipper makes it
    CHECK(clipper->AddRef() == 2);
    CHECK(clipper->Release() == 1);
    std::vector<RectL> rects;
    CHECK(clipper->GetClipList(nullptr, rects) == kDdErrNoClipList);
    CHECK(clipper->Release() == 0);
    CHECK(test::wasDestroyed(clipper));
    ComObject::SetDestroyHook(nullptr);
}

} // namespace sfcop::hle::ddraw4
