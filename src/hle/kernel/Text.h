// The game's strings at the Win32 surface: ANSI (code page 1252, taken here as Latin-1, which
// agrees on every character a file name of the game uses) for the A calls, UTF-16 for the W calls,
// and UTF-8 inside this project. Also the Win32 rule for answering a string into a caller's
// buffer.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace sfcop::hle::kernel {

// Latin-1 bytes to UTF-8.
std::string ansiToUtf8(std::string_view ansi);
// UTF-8 to Latin-1; a character above U+00FF, or a malformed sequence, becomes '?'.
std::string utf8ToAnsi(std::string_view utf8);
// UTF-16 (surrogate pairs joined; an unpaired surrogate becomes U+FFFD) to UTF-8.
std::string utf16ToUtf8(std::u16string_view utf16);
// UTF-8 to UTF-16; a malformed sequence becomes U+FFFD.
std::u16string utf8ToUtf16(std::string_view utf8);

// A NUL-terminated UTF-16 string from memory, as the game passes one; null stays empty.
std::u16string wideFrom(const std::uint16_t *text);

// The Win32 answer of a "copy the string into the buffer" call (GetCurrentDirectoryA and its
// kin): when the string and its NUL fit in `size` characters it is copied and its length without
// the NUL is returned; otherwise nothing is written and the size it needs, with the NUL, is
// returned.
std::uint32_t answerString(std::string_view text, char *buffer, std::uint32_t size);
std::uint32_t answerWide(std::u16string_view text, std::uint16_t *buffer, std::uint32_t size);

// As much of `text` as fits with a NUL in `size` characters (nothing when size is 0); returns the
// characters copied without the NUL.
std::uint32_t copyTruncated(std::string_view text, char *buffer, std::uint32_t size);
std::uint32_t copyTruncatedWide(std::u16string_view text, std::uint16_t *buffer,
                                std::uint32_t size);

} // namespace sfcop::hle::kernel
