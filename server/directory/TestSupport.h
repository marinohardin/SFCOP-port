// Test helpers: frames are written out by hand as hex text (spaces and newlines ignored).
#pragma once

#include <string>

#include "Wire.h"

namespace sfcop::directory::test {

inline Bytes hex(const std::string& text) {
    Bytes out;
    int high = -1;
    for (char c : text) {
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else continue;
        if (high < 0) {
            high = v;
        } else {
            out.push_back(std::uint8_t(high * 16 + v));
            high = -1;
        }
    }
    return out;
}

// The UTF-16LE bytes of an ASCII text, as hex, for building expected frames by hand.
inline std::string utf16(const std::string& ascii) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (char c : ascii) {
        out += digits[(unsigned char)c >> 4];
        out += digits[(unsigned char)c & 15];
        out += " 00 ";
    }
    return out;
}

inline std::string text(const Bytes& b) { return std::string(b.begin(), b.end()); }

}  // namespace sfcop::directory::test
