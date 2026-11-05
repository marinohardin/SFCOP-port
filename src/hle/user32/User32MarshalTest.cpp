// The structure layouts the USER32 rows read and write, against byte images written out by hand,
// and wsprintfA's format subset.
#include "hle/user32/User32Marshal.h"

#include <doctest/doctest.h>

#include <array>
#include <memory>
#include <vector>

namespace sfcop::hle::user32::marshal {

namespace {

std::function<std::uint32_t()> args(std::vector<std::uint32_t> words) {
    auto next = std::make_shared<std::size_t>(0);
    return [words = std::move(words), next] { return words.at((*next)++); };
}

std::string noStrings(std::uint32_t) {
    return "<string>";
}

} // namespace

TEST_CASE("RECT is four little-endian words, left top right bottom") {
    const std::uint8_t image[16] = {0x0A, 0, 0, 0, 0x14, 0, 0, 0, 0x80, 0x02, 0, 0, 0xE0, 0x01, 0, 0};
    const Rect rect = readRect(image);
    CHECK(rect.left == 10);
    CHECK(rect.top == 20);
    CHECK(rect.right == 640);
    CHECK(rect.bottom == 480);

    std::uint8_t out[16] = {};
    writeRect(out, Rect{-1, 2, 3, -4});
    const std::uint8_t expected[16] = {0xFF, 0xFF, 0xFF, 0xFF, 2, 0, 0, 0,
                                       3,    0,    0,    0,    0xFC, 0xFF, 0xFF, 0xFF};
    CHECK(std::vector<std::uint8_t>(out, out + 16) ==
          std::vector<std::uint8_t>(expected, expected + 16));
}

TEST_CASE("POINT is x then y, signed") {
    const std::uint8_t image[8] = {0xFE, 0xFF, 0xFF, 0xFF, 0x64, 0, 0, 0};
    const Point point = readPoint(image);
    CHECK(point.x == -2);
    CHECK(point.y == 100);
    std::uint8_t out[8] = {};
    writePoint(out, Point{300, -1});
    const std::uint8_t expected[8] = {0x2C, 0x01, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
    CHECK(std::vector<std::uint8_t>(out, out + 8) ==
          std::vector<std::uint8_t>(expected, expected + 8));
}

TEST_CASE("MSG is hwnd, message, wParam, lParam, time, pt.x, pt.y") {
    const std::uint8_t image[28] = {
        0x01, 0x00, 0x03, 0x00, // hwnd 0x00030001
        0x00, 0x02, 0x00, 0x00, // WM_KEYDOWN 0x200 (an invented number)
        0x41, 0x00, 0x00, 0x00, // wParam 'A'
        0x01, 0x00, 0x1E, 0x00, // lParam 0x001E0001
        0xE8, 0x03, 0x00, 0x00, // time 1000
        0x0A, 0x00, 0x00, 0x00, // pt.x 10
        0xF6, 0xFF, 0xFF, 0xFF, // pt.y -10
    };
    const Message m = readMessage(image);
    CHECK(m.hwnd == 0x00030001);
    CHECK(m.message == 0x200);
    CHECK(m.wParam == 0x41);
    CHECK(m.lParam == 0x001E0001);
    CHECK(m.time == 1000);
    CHECK(m.x == 10);
    CHECK(m.y == -10);

    std::uint8_t out[28] = {};
    writeMessage(out, m);
    CHECK(std::vector<std::uint8_t>(out, out + 28) ==
          std::vector<std::uint8_t>(image, image + 28));
}

TEST_CASE("WNDCLASSA fields sit at the offsets of the Win32 structure") {
    std::array<std::uint8_t, 40> image{};
    const std::uint32_t words[10] = {0x0003, 0x00401000, 0, 8,          0x00400000,
                                     0x11,   0x22,       6, 0x00500000, 0x00500010};
    for (std::size_t i = 0; i < 10; ++i) {
        put32(image.data() + 4 * i, words[i]);
    }
    const WndClassFields c = readWndClass(image.data());
    CHECK(c.style == 3);
    CHECK(c.proc == 0x00401000);
    CHECK(c.classExtra == 0);
    CHECK(c.windowExtra == 8);
    CHECK(c.instance == 0x00400000);
    CHECK(c.icon == 0x11);
    CHECK(c.cursor == 0x22);
    CHECK(c.background == 6);
    CHECK(c.menuName == 0x00500000);
    CHECK(c.className == 0x00500010);
}

TEST_CASE("PAINTSTRUCT holds the DC, the erase flag and the rectangle, and clears the rest") {
    std::array<std::uint8_t, 64> image;
    image.fill(0xEE);
    writePaintStruct(image.data(), 0x00040005, true, Rect{1, 2, 3, 4});
    std::array<std::uint8_t, 64> expected{};
    expected[0] = 0x05, expected[1] = 0x00, expected[2] = 0x04; // hdc
    expected[4] = 1;                                            // fErase
    expected[8] = 1, expected[12] = 2, expected[16] = 3, expected[20] = 4; // rcPaint
    CHECK(image == expected);
    CHECK(paintStructDc(image.data()) == 0x00040005);
}

TEST_CASE("DEVMODEA gets dmSize, dmFields and the four display values") {
    std::array<std::uint8_t, 156> image;
    image.fill(0x55);
    writeDevMode(image.data(), 16, 800, 600, 60);
    CHECK(image[36] == 156); // dmSize
    CHECK(image[37] == 0);
    CHECK(get32(image.data() + 40) == 0x005C0000u); // bits, width, height, frequency
    CHECK(get32(image.data() + 104) == 16);
    CHECK(get32(image.data() + 108) == 800);
    CHECK(get32(image.data() + 112) == 600);
    CHECK(get32(image.data() + 116) == 0);
    CHECK(get32(image.data() + 120) == 60);
    CHECK(image[0] == 0x55);   // the device name is left as it was
    CHECK(image[155] == 0x55); // and so is the end
}

TEST_CASE("SystemParametersInfoA: the action the game passes, and the mouse forms") {
    const Rect screen{0, 0, 640, 480};
    // The game's two calls: SPI_SCREENSAVERRUNNING (0x61) with a pointer to a BOOL.
    const SpiAnswer running = systemParametersInfo(0x61, screen);
    CHECK(running.handled);
    CHECK(running.words == 1);
    CHECK(running.value[0] == 0);
    // SPI_GETMOUSESPEED (0x70) is one UINT; the three-DWORD form is SPI_GETMOUSE (0x03).
    const SpiAnswer speed = systemParametersInfo(0x70, screen);
    CHECK(speed.words == 1);
    CHECK(speed.value[0] == 10);
    const SpiAnswer mouse = systemParametersInfo(0x03, screen);
    CHECK(mouse.words == 3);
    CHECK(mouse.value[0] == 6);
    CHECK(mouse.value[1] == 10);
    CHECK(mouse.value[2] == 1);
    const SpiAnswer area = systemParametersInfo(0x30, screen);
    CHECK(area.words == 4);
    CHECK(area.value[2] == 640);
    CHECK(area.value[3] == 480);
    CHECK_FALSE(systemParametersInfo(0x0A, screen).handled); // SPI_GETKEYBOARDSPEED: not answered
    CHECK(systemParametersInfo(0x0A, screen).words == 0);
}

TEST_CASE("wsprintfA: integers, strings, characters and percent") {
    CHECK(formatWsprintf("%d items, %u bytes", args({0xFFFFFFFD, 7}), noStrings) ==
          "-3 items, 7 bytes");
    CHECK(formatWsprintf("%x/%X", args({255, 255}), noStrings) == "ff/FF");
    CHECK(formatWsprintf("[%c] 100%%", args({'Q'}), noStrings) == "[Q] 100%");
    CHECK(formatWsprintf("name=%s", args({0x1000}), noStrings) == "name=<string>");
    CHECK(formatWsprintf("%ld %hu", args({5, 6}), noStrings) == "5 6");
}

TEST_CASE("wsprintfA: width, zero fill and left justification") {
    CHECK(formatWsprintf("%5d|", args({42}), noStrings) == "   42|");
    CHECK(formatWsprintf("%05d|", args({42}), noStrings) == "00042|");
    CHECK(formatWsprintf("%05d|", args({0xFFFFFFFE}), noStrings) == "-0002|");
    CHECK(formatWsprintf("%-5d|", args({42}), noStrings) == "42   |");
    CHECK(formatWsprintf("%6s|", args({1}), noStrings) == "<string>|"); // wider than 6: unchanged
    CHECK(formatWsprintf("%10s|", args({1}), noStrings) == "  <string>|");
}

TEST_CASE("wsprintfA: what it does not know is copied, and the result is bounded") {
    CHECK(formatWsprintf("100%", args({}), noStrings) == "100%");
    CHECK(formatWsprintf("%q", args({}), noStrings) == "%q");
    const std::string longText(2000, 'a');
    CHECK(formatWsprintf(longText, args({}), noStrings).size() == 1023);
}

} // namespace sfcop::hle::user32::marshal
