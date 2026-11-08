// A small JSON reader and writer for the lobby's bodies (LOBBY_API.md, "Bodies"): UTF-8, at most
// 4 KB, objects with scalar fields and arrays of integers. Numbers that are not integers parse as
// Kind::Real so a field that must be an integer can refuse them.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace sfcop::lobby {

struct Json {
    enum class Kind { Null, Bool, Integer, Real, String, Array, Object };

    Kind kind = Kind::Null;
    bool boolean = false;
    int64_t integer = 0;
    std::string string;
    std::vector<Json> array;
    std::map<std::string, Json> object;

    static Json makeBool(bool v);
    static Json makeInt(int64_t v);
    static Json makeString(std::string v);
    static Json makeArray();
    static Json makeObject();

    // Object field by name, or nullptr (also when this is not an object).
    const Json* find(const std::string& name) const;
    Json& set(const std::string& name, Json value);
    Json& push(Json value);

    // Compact text, keys in sorted order.
    std::string dump() const;
};

// Parses exactly one value with nothing after it but white space. Nesting deeper than 8 and
// invalid UTF-8 or escapes are refused.
std::optional<Json> parseJson(const std::string& text);

bool isValidUtf8(const std::string& text);

} // namespace sfcop::lobby
