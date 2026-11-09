// MD5 (RFC 1321), only because the original account login is built on it
// (src/net/DIRECTORY_API.md, "Account login"). It protects nothing here; see "Security" there.
#pragma once

#include <cstdint>
#include <string>

namespace sfcop::auth {

// The digest of `data` as 32 lowercase hexadecimal digits.
std::string md5Hex(const std::string& data);

}  // namespace sfcop::auth
