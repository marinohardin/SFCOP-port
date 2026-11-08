#include "Json.h"

#include <cstdio>
#include <cstdlib>

namespace sfcop::lobby {

Json Json::makeBool(bool v) {
    Json j;
    j.kind = Kind::Bool;
    j.boolean = v;
    return j;
}
Json Json::makeInt(int64_t v) {
    Json j;
    j.kind = Kind::Integer;
    j.integer = v;
    return j;
}
Json Json::makeString(std::string v) {
    Json j;
    j.kind = Kind::String;
    j.string = std::move(v);
    return j;
}
Json Json::makeArray() {
    Json j;
    j.kind = Kind::Array;
    return j;
}
Json Json::makeObject() {
    Json j;
    j.kind = Kind::Object;
    return j;
}

const Json* Json::find(const std::string& name) const {
    if (kind != Kind::Object) return nullptr;
    auto it = object.find(name);
    return it == object.end() ? nullptr : &it->second;
}
Json& Json::set(const std::string& name, Json value) { return object[name] = std::move(value); }
Json& Json::push(Json value) {
    array.push_back(std::move(value));
    return array.back();
}

namespace {

void dumpString(const std::string& s, std::string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += char(c);
            }
        }
    }
    out += '"';
}

void dumpInto(const Json& j, std::string& out) {
    switch (j.kind) {
    case Json::Kind::Null: out += "null"; break;
    case Json::Kind::Bool: out += j.boolean ? "true" : "false"; break;
    case Json::Kind::Integer: out += std::to_string(j.integer); break;
    case Json::Kind::Real: out += "0"; break; // never produced by the lobby
    case Json::Kind::String: dumpString(j.string, out); break;
    case Json::Kind::Array: {
        out += '[';
        bool first = true;
        for (const Json& e : j.array) {
            if (!first) out += ',';
            first = false;
            dumpInto(e, out);
        }
        out += ']';
        break;
    }
    case Json::Kind::Object: {
        out += '{';
        bool first = true;
        for (const auto& kv : j.object) {
            if (!first) out += ',';
            first = false;
            dumpString(kv.first, out);
            out += ':';
            dumpInto(kv.second, out);
        }
        out += '}';
        break;
    }
    }
}

class Parser {
public:
    explicit Parser(const std::string& t) : text_(t) {}

    std::optional<Json> parseDocument() {
        skipSpace();
        auto v = parseValue(0);
        if (!v) return std::nullopt;
        skipSpace();
        if (pos_ != text_.size()) return std::nullopt;
        return v;
    }

private:
    const std::string& text_;
    size_t pos_ = 0;

    bool atEnd() const { return pos_ >= text_.size(); }
    char peek() const { return text_[pos_]; }
    void skipSpace() {
        while (!atEnd() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r')) ++pos_;
    }
    bool consume(const char* word) {
        size_t n = 0;
        while (word[n]) ++n;
        if (text_.compare(pos_, n, word) != 0) return false;
        pos_ += n;
        return true;
    }

    std::optional<Json> parseValue(int depth) {
        if (depth > 8 || atEnd()) return std::nullopt;
        char c = peek();
        if (c == '{') return parseObject(depth);
        if (c == '[') return parseArray(depth);
        if (c == '"') {
            auto s = parseString();
            if (!s) return std::nullopt;
            return Json::makeString(std::move(*s));
        }
        if (c == 't') return consume("true") ? std::optional<Json>(Json::makeBool(true)) : std::nullopt;
        if (c == 'f') return consume("false") ? std::optional<Json>(Json::makeBool(false)) : std::nullopt;
        if (c == 'n') return consume("null") ? std::optional<Json>(Json()) : std::nullopt;
        return parseNumber();
    }

    std::optional<Json> parseNumber() {
        size_t start = pos_;
        bool real = false;
        if (!atEnd() && peek() == '-') ++pos_;
        size_t digits = 0;
        while (!atEnd() && peek() >= '0' && peek() <= '9') {
            ++pos_;
            ++digits;
        }
        if (digits == 0) return std::nullopt;
        if (text_[start] == '0' && digits > 1) return std::nullopt;
        if (text_[start] == '-' && text_[start + 1] == '0' && digits > 1) return std::nullopt;
        if (!atEnd() && peek() == '.') {
            real = true;
            ++pos_;
            size_t frac = 0;
            while (!atEnd() && peek() >= '0' && peek() <= '9') {
                ++pos_;
                ++frac;
            }
            if (frac == 0) return std::nullopt;
        }
        if (!atEnd() && (peek() == 'e' || peek() == 'E')) {
            real = true;
            ++pos_;
            if (!atEnd() && (peek() == '+' || peek() == '-')) ++pos_;
            size_t exp = 0;
            while (!atEnd() && peek() >= '0' && peek() <= '9') {
                ++pos_;
                ++exp;
            }
            if (exp == 0) return std::nullopt;
        }
        Json j;
        if (real) {
            j.kind = Json::Kind::Real;
            return j;
        }
        if (digits > 18) { // beyond int64: kept as Real so range checks refuse it
            j.kind = Json::Kind::Real;
            return j;
        }
        j.kind = Json::Kind::Integer;
        j.integer = std::strtoll(text_.substr(start, pos_ - start).c_str(), nullptr, 10);
        return j;
    }

    static void appendUtf8(uint32_t cp, std::string& out) {
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }

    std::optional<uint32_t> parseHex4() {
        if (pos_ + 4 > text_.size()) return std::nullopt;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = text_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
            else return std::nullopt;
        }
        return v;
    }

    std::optional<std::string> parseString() {
        ++pos_; // opening quote
        std::string out;
        while (true) {
            if (atEnd()) return std::nullopt;
            unsigned char c = static_cast<unsigned char>(text_[pos_++]);
            if (c == '"') break;
            if (c < 0x20) return std::nullopt;
            if (c != '\\') {
                out += char(c);
                continue;
            }
            if (atEnd()) return std::nullopt;
            char e = text_[pos_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                auto cp = parseHex4();
                if (!cp) return std::nullopt;
                if (*cp >= 0xD800 && *cp < 0xDC00) {
                    if (!consume("\\u")) return std::nullopt;
                    auto lo = parseHex4();
                    if (!lo || *lo < 0xDC00 || *lo > 0xDFFF) return std::nullopt;
                    *cp = 0x10000 + ((*cp - 0xD800) << 10) + (*lo - 0xDC00);
                } else if (*cp >= 0xDC00 && *cp <= 0xDFFF) {
                    return std::nullopt;
                }
                appendUtf8(*cp, out);
                break;
            }
            default: return std::nullopt;
            }
        }
        if (!isValidUtf8(out)) return std::nullopt;
        return out;
    }

    std::optional<Json> parseArray(int depth) {
        ++pos_;
        Json j = Json::makeArray();
        skipSpace();
        if (!atEnd() && peek() == ']') {
            ++pos_;
            return j;
        }
        while (true) {
            skipSpace();
            auto v = parseValue(depth + 1);
            if (!v) return std::nullopt;
            j.push(std::move(*v));
            skipSpace();
            if (atEnd()) return std::nullopt;
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == ']') {
                ++pos_;
                return j;
            }
            return std::nullopt;
        }
    }

    std::optional<Json> parseObject(int depth) {
        ++pos_;
        Json j = Json::makeObject();
        skipSpace();
        if (!atEnd() && peek() == '}') {
            ++pos_;
            return j;
        }
        while (true) {
            skipSpace();
            if (atEnd() || peek() != '"') return std::nullopt;
            auto key = parseString();
            if (!key) return std::nullopt;
            skipSpace();
            if (atEnd() || peek() != ':') return std::nullopt;
            ++pos_;
            skipSpace();
            auto v = parseValue(depth + 1);
            if (!v) return std::nullopt;
            j.object[*key] = std::move(*v);
            skipSpace();
            if (atEnd()) return std::nullopt;
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == '}') {
                ++pos_;
                return j;
            }
            return std::nullopt;
        }
    }
};

} // namespace

std::string Json::dump() const {
    std::string out;
    dumpInto(*this, out);
    return out;
}

std::optional<Json> parseJson(const std::string& text) { return Parser(text).parseDocument(); }

bool isValidUtf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n;
        uint32_t cp;
        if (c < 0x80) {
            ++i;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            n = 1;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            n = 2;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            n = 3;
            cp = c & 0x07;
        } else {
            return false;
        }
        for (size_t k = 1; k <= n; ++k) {
            if (i + k >= s.size()) return false;
            unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000)) return false;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += n + 1;
    }
    return true;
}

} // namespace sfcop::lobby
