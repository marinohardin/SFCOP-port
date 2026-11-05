#include "hle/user32/Keys.h"

#include <doctest/doctest.h>

namespace sfcop::hle::user32 {

TEST_CASE("A HID usage maps to its virtual key and set-1 scan code") {
    const auto a = keyFromUsage(0x04);
    REQUIRE(a);
    CHECK(a->vk == 'A');
    CHECK(a->sideVk == 'A');
    CHECK(a->scan == 0x1E);
    CHECK_FALSE(a->extended);

    const auto leftShift = keyFromUsage(0xE1);
    REQUIRE(leftShift);
    CHECK(leftShift->vk == 0x10);     // VK_SHIFT in the message
    CHECK(leftShift->sideVk == 0xA0); // VK_LSHIFT for GetAsyncKeyState
    CHECK(leftShift->scan == 0x2A);

    const auto rightControl = keyFromUsage(0xE4);
    REQUIRE(rightControl);
    CHECK(rightControl->vk == 0x11);
    CHECK(rightControl->sideVk == 0xA3);
    CHECK(rightControl->scan == 0x1D);
    CHECK(rightControl->extended);

    const auto up = keyFromUsage(0x52);
    REQUIRE(up);
    CHECK(up->vk == 0x26);
    CHECK(up->scan == 0x48);
    CHECK(up->extended);

    const auto f10 = keyFromUsage(0x43);
    REQUIRE(f10);
    CHECK(f10->vk == 0x79);
    CHECK(f10->scan == 0x44);
}

TEST_CASE("A usage the table does not list maps to nothing") {
    CHECK_FALSE(keyFromUsage(0x00));
    CHECK_FALSE(keyFromUsage(0xFF));
}

TEST_CASE("A side-less virtual key reads both of its keys") {
    platform::Scancode usages[2] = {};
    CHECK(usagesOfVk(0x10, usages) == 2);
    CHECK(usages[0] == 0xE1);
    CHECK(usages[1] == 0xE5);
    CHECK(usagesOfVk(0xA2, usages) == 1); // VK_LCONTROL
    CHECK(usages[0] == 0xE0);
    CHECK(usagesOfVk('Q', usages) == 1);
    CHECK(usages[0] == 0x14);
    CHECK(usagesOfVk(0xFF, usages) == 0);
}

TEST_CASE("WM_KEYDOWN and WM_KEYUP's lParam carry the scan code and the transition") {
    const KeyCode a{'A', 'A', 0x1E, false};
    CHECK(keyLParam(a, true, false) == 0x001E0001u);
    CHECK(keyLParam(a, true, true) == 0x401E0001u);
    CHECK(keyLParam(a, false, false) == 0xC01E0001u);
    const KeyCode up{0x26, 0x26, 0x48, true};
    CHECK(keyLParam(up, true, false) == 0x01480001u);
}

TEST_CASE("Characters follow a US keyboard with Shift, Control and Caps Lock") {
    CHECK(charFromVk('A', false, false, false) == 'a');
    CHECK(charFromVk('A', true, false, false) == 'A');
    CHECK(charFromVk('A', false, false, true) == 'A');
    CHECK(charFromVk('A', true, false, true) == 'a');
    CHECK(charFromVk('C', false, true, false) == 0x03);
    CHECK(charFromVk('1', false, false, false) == '1');
    CHECK(charFromVk('1', true, false, false) == '!');
    CHECK(charFromVk('0', true, false, false) == ')');
    CHECK(charFromVk(0xBD, false, false, false) == '-');
    CHECK(charFromVk(0xBD, true, false, false) == '_');
    CHECK(charFromVk(0x0D, false, false, false) == '\r');
    CHECK(charFromVk(0x08, false, false, false) == '\b');
    CHECK(charFromVk(0x20, true, false, false) == ' ');
    CHECK(charFromVk(0x26, false, false, false) == 0); // an arrow types nothing
    CHECK(charFromVk(0x10, false, false, false) == 0);
}

TEST_CASE("MapVirtualKeyA's four map types") {
    CHECK(mapVirtualKey('A', 0) == 0x1E);
    CHECK(mapVirtualKey(0x1E, 1) == 'A');
    CHECK(mapVirtualKey('A', 2) == 'A');
    CHECK(mapVirtualKey(0xBD, 2) == '-');
    CHECK(mapVirtualKey(0x2A, 3) == 0xA0);
    CHECK(mapVirtualKey(0x36, 3) == 0xA1);
    CHECK(mapVirtualKey(0x79, 0) == 0x44);
    CHECK(mapVirtualKey(0xFF, 0) == 0);
    CHECK(mapVirtualKey('A', 7) == 0);
}

TEST_CASE("VkKeyScanA gives the key and the shift state that type a character") {
    CHECK(vkKeyScan('a') == 0x0041u);
    CHECK(vkKeyScan('A') == 0x0141u);
    CHECK(vkKeyScan('!') == 0x0131u);
    CHECK(vkKeyScan('_') == 0x01BDu);
    CHECK(vkKeyScan(' ') == 0x0020u);
    CHECK(vkKeyScan('\r') == 0x000Du);
    CHECK(vkKeyScan('\b') == 0x0008u);
    CHECK(vkKeyScan('\t') == 0x0009u);
    CHECK(vkKeyScan(0x01) == 0x0241u); // Ctrl+A
    CHECK(vkKeyScan(static_cast<char>(0xE9)) == 0xFFFFu);
}

} // namespace sfcop::hle::user32
