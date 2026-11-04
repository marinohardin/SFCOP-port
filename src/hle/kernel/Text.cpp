#include "hle/kernel/Text.h"

#include <cstring>

namespace sfcop::hle::kernel {
namespace {

constexpr char32_t kReplacement = 0xFFFD;

void appendUtf8(std::string &out, char32_t c) {
    if (c < 0x80) {
        out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<char>(0xC0 | c >> 6));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | c >> 12));
        out.push_back(static_cast<char>(0x80 | (c >> 6 & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | c >> 18));
        out.push_back(static_cast<char>(0x80 | (c >> 12 & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c >> 6 & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

// The next code point of `text` from `at`, which it advances; kReplacement for a malformed
// sequence (one byte is consumed).
char32_t nextUtf8(std::string_view text, std::size_t &at) {
    auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    unsigned char lead = byte(at);
    if (lead < 0x80) {
        at += 1;
        return lead;
    }
    int extra = lead >= 0xF5 ? -1 : lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : lead >= 0xC2 ? 1 : -1;
    if (extra < 0 || at + static_cast<std::size_t>(extra) >= text.size()) {
        at += 1;
        return kReplacement;
    }
    char32_t c = lead & (0x3F >> extra);
    for (int i = 1; i <= extra; i++) {
        unsigned char next = byte(at + static_cast<std::size_t>(i));
        if ((next & 0xC0) != 0x80) {
            at += 1;
            return kReplacement;
        }
        c = c << 6 | (next & 0x3F);
    }
    at += static_cast<std::size_t>(extra) + 1;
    static const char32_t kSmallest[] = {0, 0x80, 0x800, 0x10000};
    if (c < kSmallest[extra] || c > 0x10FFFF || (c >= 0xD800 && c < 0xE000)) {
        return kReplacement;
    }
    return c;
}

} // namespace

std::string ansiToUtf8(std::string_view ansi) {
    std::string out;
    out.reserve(ansi.size());
    for (char c : ansi) {
        appendUtf8(out, static_cast<unsigned char>(c));
    }
    return out;
}

std::string utf8ToAnsi(std::string_view utf8) {
    std::string out;
    out.reserve(utf8.size());
    for (std::size_t at = 0; at < utf8.size();) {
        char32_t c = nextUtf8(utf8, at);
        out.push_back(c <= 0xFF ? static_cast<char>(c) : '?');
    }
    return out;
}

std::string utf16ToUtf8(std::u16string_view utf16) {
    std::string out;
    out.reserve(utf16.size());
    for (std::size_t i = 0; i < utf16.size(); i++) {
        char32_t c = utf16[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < utf16.size() && utf16[i + 1] >= 0xDC00 &&
            utf16[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (utf16[i + 1] - 0xDC00);
            i++;
        } else if (c >= 0xD800 && c < 0xE000) {
            c = kReplacement;
        }
        appendUtf8(out, c);
    }
    return out;
}

std::u16string utf8ToUtf16(std::string_view utf8) {
    std::u16string out;
    out.reserve(utf8.size());
    for (std::size_t at = 0; at < utf8.size();) {
        char32_t c = nextUtf8(utf8, at);
        if (c >= 0x10000) {
            c -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (c >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (c & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(c));
        }
    }
    return out;
}

std::u16string wideFrom(const std::uint16_t *text) {
    std::u16string out;
    if (text == nullptr) {
        return out;
    }
    for (; *text != 0; text++) {
        out.push_back(static_cast<char16_t>(*text));
    }
    return out;
}

std::uint32_t answerString(std::string_view text, char *buffer, std::uint32_t size) {
    auto length = static_cast<std::uint32_t>(text.size());
    if (buffer == nullptr || length >= size) {
        return length + 1;
    }
    std::memcpy(buffer, text.data(), length);
    buffer[length] = '\0';
    return length;
}

std::uint32_t answerWide(std::u16string_view text, std::uint16_t *buffer, std::uint32_t size) {
    auto length = static_cast<std::uint32_t>(text.size());
    if (buffer == nullptr || length >= size) {
        return length + 1;
    }
    for (std::uint32_t i = 0; i < length; i++) {
        buffer[i] = text[i];
    }
    buffer[length] = 0;
    return length;
}

std::uint32_t copyTruncated(std::string_view text, char *buffer, std::uint32_t size) {
    if (buffer == nullptr || size == 0) {
        return 0;
    }
    std::uint32_t count = static_cast<std::uint32_t>(text.size()) < size - 1
                              ? static_cast<std::uint32_t>(text.size())
                              : size - 1;
    std::memcpy(buffer, text.data(), count);
    buffer[count] = '\0';
    return count;
}

std::uint32_t copyTruncatedWide(std::u16string_view text, std::uint16_t *buffer,
                                std::uint32_t size) {
    if (buffer == nullptr || size == 0) {
        return 0;
    }
    std::uint32_t count = static_cast<std::uint32_t>(text.size()) < size - 1
                              ? static_cast<std::uint32_t>(text.size())
                              : size - 1;
    for (std::uint32_t i = 0; i < count; i++) {
        buffer[i] = text[i];
    }
    buffer[count] = 0;
    return count;
}

} // namespace sfcop::hle::kernel
