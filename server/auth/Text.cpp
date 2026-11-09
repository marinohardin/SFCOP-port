#include "Text.h"

namespace sfcop::auth {
namespace {

const std::string kFinal = "\\final\\";

bool parseOne(const std::string& body, Message& out) {
    // body: "\k1\v1\k2\v2" (the `\final\` already cut). A message with a trailing key and no value
    // (`\logout\`) gets an empty value.
    if (body.empty() || body[0] != '\\') return false;
    std::vector<std::string> parts;
    std::size_t start = 1;
    for (;;) {
        std::size_t next = body.find('\\', start);
        if (next == std::string::npos) {
            parts.push_back(body.substr(start));
            break;
        }
        parts.push_back(body.substr(start, next - start));
        start = next + 1;
    }
    if (parts.size() % 2 == 1) parts.push_back(std::string());
    Message m;
    for (std::size_t i = 0; i < parts.size(); i += 2) {
        if (parts[i].empty()) return false;
        m.pairs.emplace_back(parts[i], parts[i + 1]);
    }
    out = std::move(m);
    return true;
}

}  // namespace

bool Message::get(const std::string& key, std::string& value) const {
    for (std::size_t i = 1; i < pairs.size(); ++i) {
        if (pairs[i].first == key) {
            value = pairs[i].second;
            return true;
        }
    }
    return false;
}

bool takeMessages(std::string& buffer, std::vector<Message>& out) {
    for (;;) {
        std::size_t end = buffer.find(kFinal);
        if (end == std::string::npos) return buffer.size() <= kMaxMessage;
        if (end > kMaxMessage) return false;
        Message m;
        if (!parseOne(buffer.substr(0, end), m)) return false;
        out.push_back(std::move(m));
        buffer.erase(0, end + kFinal.size());
    }
}

std::string buildMessage(const std::vector<std::pair<std::string, std::string>>& pairs) {
    std::string out;
    for (const auto& [key, value] : pairs) {
        out += '\\';
        out += key;
        out += '\\';
        for (char c : value) {
            if (c != '\\') out += c;
        }
    }
    return out + kFinal;
}

}  // namespace sfcop::auth
