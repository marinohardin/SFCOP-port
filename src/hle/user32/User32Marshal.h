// User32Marshal: how the USER32 rows read and write the game's structures (MSG, RECT, POINT,
// WNDCLASSA, PAINTSTRUCT, DEVMODEA) and format wsprintfA's output. Pure functions on byte images
// in the guest's layout (little-endian, 32-bit fields), so that the layouts are tested without the
// lift; the guest shims (User32Guest.cpp) call them on the game's memory.
#pragma once

#include "hle/user32/MessageQueue.h"
#include "hle/user32/WindowManager.h"

#include <cstdint>
#include <functional>
#include <string>

namespace sfcop::hle::user32::marshal {

// ---- words ----

std::uint32_t get32(const std::uint8_t *at);
void put32(std::uint8_t *at, std::uint32_t value);
void put16(std::uint8_t *at, std::uint16_t value);

// ---- RECT (left, top, right, bottom) and POINT (x, y) ----

constexpr std::uint32_t kRectSize = 16;
constexpr std::uint32_t kPointSize = 8;
Rect readRect(const std::uint8_t *at);
void writeRect(std::uint8_t *at, const Rect &rect);

struct Point {
    std::int32_t x = 0;
    std::int32_t y = 0;
};
Point readPoint(const std::uint8_t *at);
void writePoint(std::uint8_t *at, Point point);

// ---- MSG: hwnd, message, wParam, lParam, time, pt.x, pt.y ----

constexpr std::uint32_t kMsgSize = 28;
Message readMessage(const std::uint8_t *at);
void writeMessage(std::uint8_t *at, const Message &message);

// ---- WNDCLASSA ----

constexpr std::uint32_t kWndClassSize = 40;
// The fields as stored: the menu and class names are guest addresses (or atoms), left to the
// caller to read.
struct WndClassFields {
    std::uint32_t style = 0;
    std::uint32_t proc = 0;
    std::int32_t classExtra = 0;
    std::int32_t windowExtra = 0;
    std::uint32_t instance = 0;
    std::uint32_t icon = 0;
    std::uint32_t cursor = 0;
    std::uint32_t background = 0;
    std::uint32_t menuName = 0;
    std::uint32_t className = 0;
};
WndClassFields readWndClass(const std::uint8_t *at);

// ---- PAINTSTRUCT: hdc, fErase, rcPaint, fRestore, fIncUpdate, rgbReserved[32] ----

constexpr std::uint32_t kPaintStructSize = 64;
void writePaintStruct(std::uint8_t *at, std::uint32_t dc, bool erase, const Rect &paint);
std::uint32_t paintStructDc(const std::uint8_t *at);

// ---- DEVMODEA, as EnumDisplaySettingsA fills it ----

constexpr std::uint32_t kDevModeSize = 156;
// Sets dmSize, dmFields (bits per pel, width, height, frequency) and those four values; the rest
// of the structure is left as the caller had it.
void writeDevMode(std::uint8_t *at, std::uint32_t bitsPerPixel, std::uint32_t width,
                  std::uint32_t height, std::uint32_t refreshHz);

// ---- SystemParametersInfoA ----

// What an action writes through pvParam: `words` 32-bit words, or a RECT (4 words) for the work area.
struct SpiAnswer {
    bool handled = false; // false: the action is not one this module knows (ERROR_INVALID_PARAMETER)
    std::uint32_t words = 0;
    std::uint32_t value[4] = {};
};
// SPI_GETWORKAREA (the screen), SPI_SETSCREENSAVERRUNNING and SPI_GETSCREENSAVEACTIVE (not
// running, not active: one word, 0), SPI_GETMOUSESPEED (one UINT, the default 10) and
// SPI_GETMOUSE (three DWORDs, 6, 10 and 1).
SpiAnswer systemParametersInfo(std::uint32_t action, const Rect &screen);

// ---- wsprintfA ----

constexpr std::size_t kWsprintfLimit = 1024; // wsprintfA writes at most this many bytes, with the NUL

// The text wsprintfA makes: %d %i %u %x %X %c %s %%, with the '-' and '0' flags, a width, and the
// 'l' and 'h' size prefixes (all arguments are 32-bit words). Anything else is copied as it
// stands. `nextArg` gives the next word of the caller's arguments; `readString` the string at a
// guest address (empty for a null one). The result holds at most kWsprintfLimit - 1 characters.
std::string formatWsprintf(const std::string &format, const std::function<std::uint32_t()> &nextArg,
                           const std::function<std::string(std::uint32_t)> &readString);

} // namespace sfcop::hle::user32::marshal
