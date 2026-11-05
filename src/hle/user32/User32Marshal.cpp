#include "hle/user32/User32Marshal.h"

#include "hle/user32/User32Defs.h"

#include <algorithm>
#include <cstdio>

namespace sfcop::hle::user32::marshal {

namespace {

std::int32_t s32(std::uint32_t value) {
    return static_cast<std::int32_t>(value);
}

std::uint32_t u32(std::int32_t value) {
    return static_cast<std::uint32_t>(value);
}

// DEVMODEA offsets.
constexpr std::uint32_t kDmSize = 36;
constexpr std::uint32_t kDmFields = 40;
constexpr std::uint32_t kDmBitsPerPel = 104;
constexpr std::uint32_t kDmPelsWidth = 108;
constexpr std::uint32_t kDmPelsHeight = 112;
constexpr std::uint32_t kDmDisplayFlags = 116;
constexpr std::uint32_t kDmDisplayFrequency = 120;
constexpr std::uint32_t kDmFieldsSet = 0x005C0000u; // BITSPERPEL | PELSWIDTH | PELSHEIGHT | DISPLAYFREQUENCY

} // namespace

std::uint32_t get32(const std::uint8_t *at) {
    return static_cast<std::uint32_t>(at[0]) | static_cast<std::uint32_t>(at[1]) << 8 |
           static_cast<std::uint32_t>(at[2]) << 16 | static_cast<std::uint32_t>(at[3]) << 24;
}

void put32(std::uint8_t *at, std::uint32_t value) {
    at[0] = static_cast<std::uint8_t>(value);
    at[1] = static_cast<std::uint8_t>(value >> 8);
    at[2] = static_cast<std::uint8_t>(value >> 16);
    at[3] = static_cast<std::uint8_t>(value >> 24);
}

void put16(std::uint8_t *at, std::uint16_t value) {
    at[0] = static_cast<std::uint8_t>(value);
    at[1] = static_cast<std::uint8_t>(value >> 8);
}

Rect readRect(const std::uint8_t *at) {
    return Rect{s32(get32(at)), s32(get32(at + 4)), s32(get32(at + 8)), s32(get32(at + 12))};
}

void writeRect(std::uint8_t *at, const Rect &rect) {
    put32(at, u32(rect.left));
    put32(at + 4, u32(rect.top));
    put32(at + 8, u32(rect.right));
    put32(at + 12, u32(rect.bottom));
}

Point readPoint(const std::uint8_t *at) {
    return Point{s32(get32(at)), s32(get32(at + 4))};
}

void writePoint(std::uint8_t *at, Point point) {
    put32(at, u32(point.x));
    put32(at + 4, u32(point.y));
}

Message readMessage(const std::uint8_t *at) {
    Message m;
    m.hwnd = get32(at);
    m.message = get32(at + 4);
    m.wParam = get32(at + 8);
    m.lParam = get32(at + 12);
    m.time = get32(at + 16);
    m.x = s32(get32(at + 20));
    m.y = s32(get32(at + 24));
    return m;
}

void writeMessage(std::uint8_t *at, const Message &m) {
    const std::uint32_t words[] = {m.hwnd, m.message, m.wParam, m.lParam, m.time, u32(m.x),
                                   u32(m.y)};
    for (std::uint32_t i = 0; i < 7; ++i) {
        put32(at + 4 * i, words[i]);
    }
}

WndClassFields readWndClass(const std::uint8_t *at) {
    WndClassFields c;
    c.style = get32(at);
    c.proc = get32(at + 4);
    c.classExtra = s32(get32(at + 8));
    c.windowExtra = s32(get32(at + 12));
    c.instance = get32(at + 16);
    c.icon = get32(at + 20);
    c.cursor = get32(at + 24);
    c.background = get32(at + 28);
    c.menuName = get32(at + 32);
    c.className = get32(at + 36);
    return c;
}

void writePaintStruct(std::uint8_t *at, std::uint32_t dc, bool erase, const Rect &paint) {
    put32(at, dc);
    put32(at + 4, erase ? 1 : 0);
    writeRect(at + 8, paint);
    for (std::uint32_t offset = 24; offset < kPaintStructSize; offset += 4) {
        put32(at + offset, 0); // fRestore, fIncUpdate, rgbReserved
    }
}

std::uint32_t paintStructDc(const std::uint8_t *at) {
    return get32(at);
}

void writeDevMode(std::uint8_t *at, std::uint32_t bitsPerPixel, std::uint32_t width,
                  std::uint32_t height, std::uint32_t refreshHz) {
    put16(at + kDmSize, static_cast<std::uint16_t>(kDevModeSize));
    put32(at + kDmFields, kDmFieldsSet);
    put32(at + kDmBitsPerPel, bitsPerPixel);
    put32(at + kDmPelsWidth, width);
    put32(at + kDmPelsHeight, height);
    put32(at + kDmDisplayFlags, 0);
    put32(at + kDmDisplayFrequency, refreshHz);
}

SpiAnswer systemParametersInfo(std::uint32_t action, const Rect &screen) {
    SpiAnswer answer;
    switch (action) {
    case kSpiGetWorkArea:
        answer.handled = true, answer.words = 4;
        answer.value[0] = u32(screen.left), answer.value[1] = u32(screen.top);
        answer.value[2] = u32(screen.right), answer.value[3] = u32(screen.bottom);
        break;
    case kSpiSetScreenSaverRunning: // the previous state: not running
    case kSpiGetScreenSaveActive:
        answer.handled = true, answer.words = 1;
        answer.value[0] = 0;
        break;
    case kSpiGetMouseSpeed:
        answer.handled = true, answer.words = 1;
        answer.value[0] = 10;
        break;
    case kSpiGetMouse:
        answer.handled = true, answer.words = 3;
        answer.value[0] = 6, answer.value[1] = 10, answer.value[2] = 1;
        break;
    default:
        break;
    }
    return answer;
}

std::string formatWsprintf(const std::string &format, const std::function<std::uint32_t()> &nextArg,
                           const std::function<std::string(std::uint32_t)> &readString) {
    std::string out;
    for (std::size_t i = 0; i < format.size(); ++i) {
        if (format[i] != '%') {
            out += format[i];
            continue;
        }
        const std::size_t start = i;
        ++i;
        bool left = false, zero = false;
        for (; i < format.size() && (format[i] == '-' || format[i] == '0'); ++i) {
            (format[i] == '-' ? left : zero) = true;
        }
        std::size_t width = 0;
        for (; i < format.size() && format[i] >= '0' && format[i] <= '9'; ++i) {
            width = std::min<std::size_t>(width * 10 + static_cast<std::size_t>(format[i] - '0'),
                                          kWsprintfLimit);
        }
        for (; i < format.size() && (format[i] == 'l' || format[i] == 'h'); ++i) {
        }
        if (i >= format.size()) {
            out += format.substr(start); // a lone '%' or an unfinished specification
            break;
        }
        std::string body;
        bool numeric = false;
        char buffer[16];
        switch (format[i]) {
        case 'd':
        case 'i':
            std::snprintf(buffer, sizeof buffer, "%d", s32(nextArg()));
            body = buffer, numeric = true;
            break;
        case 'u':
            std::snprintf(buffer, sizeof buffer, "%u", nextArg());
            body = buffer, numeric = true;
            break;
        case 'x':
            std::snprintf(buffer, sizeof buffer, "%x", nextArg());
            body = buffer, numeric = true;
            break;
        case 'X':
            std::snprintf(buffer, sizeof buffer, "%X", nextArg());
            body = buffer, numeric = true;
            break;
        case 'c':
            body = std::string(1, static_cast<char>(nextArg()));
            break;
        case 's':
            body = readString(nextArg());
            break;
        case '%':
            body = "%";
            break;
        default:
            out += format.substr(start, i - start + 1); // not a specification wsprintf knows
            continue;
        }
        if (body.size() < width) {
            const std::size_t pad = width - body.size();
            if (left) {
                body.append(pad, ' ');
            } else if (zero && numeric) {
                const std::size_t sign = !body.empty() && body[0] == '-' ? 1 : 0;
                body.insert(sign, pad, '0');
            } else {
                body.insert(0, pad, ' ');
            }
        }
        out += body;
        if (out.size() >= kWsprintfLimit - 1) {
            break;
        }
    }
    if (out.size() > kWsprintfLimit - 1) {
        out.resize(kWsprintfLimit - 1);
    }
    return out;
}

} // namespace sfcop::hle::user32::marshal
