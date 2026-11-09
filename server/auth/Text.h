// The original's text messages: `\key\value` pairs ending in `\final\`
// (src/net/DIRECTORY_API.md, "Conventions").
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace sfcop::auth {

constexpr std::size_t kMaxMessage = 4096;

// The pairs of one message, `\final\` excluded, in order. The first pair is the command: its
// key names it (`login`, `newuser`, `valid`) and its value is usually empty.
struct Message {
    std::vector<std::pair<std::string, std::string>> pairs;
    std::string command() const { return pairs.empty() ? std::string() : pairs.front().first; }
    // The value of the first pair named `key` after the command; false if absent.
    bool get(const std::string& key, std::string& value) const;
};

// Cuts complete messages off the front of `buffer`. False when the buffer holds more than
// kMaxMessage bytes without a `\final\` or a message is not `\key\value` pairs.
bool takeMessages(std::string& buffer, std::vector<Message>& out);

// Builds `\k1\v1\k2\v2...\final\`. A backslash in a value is dropped, which the original's
// format cannot carry.
std::string buildMessage(const std::vector<std::pair<std::string, std::string>>& pairs);

}  // namespace sfcop::auth
