// The call recorder every <Name>Mock.h uses (tests only). A mock records each call as one line,
// `Name(arg, arg)`, so a test can compare the whole sequence with a hand-written list. Scalars,
// enums (as their number), strings (quoted) and handles (their number) are written out; any other
// argument is written `{}` and the mock keeps it whole in a `last...` member instead.
#pragma once

#include "platform/Core/Handle.h"

#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace sfcop::platform {

class MockLog {
public:
    template <typename... Args> void Record(std::string_view name, const Args &...args) {
        std::ostringstream line;
        line << name << '(';
        const char *separator = "";
        ((line << separator, appendArg(line, args), separator = ", "), ...);
        line << ')';
        calls_.push_back(line.str());
    }

    const std::vector<std::string> &Calls() const { return calls_; }

    // How many recorded calls were to `name`.
    std::size_t Count(std::string_view name) const {
        std::size_t count = 0;
        for (const std::string &call : calls_) {
            if (call.size() > name.size() && call.compare(0, name.size(), name) == 0 &&
                call[name.size()] == '(') {
                ++count;
            }
        }
        return count;
    }

    void Clear() { calls_.clear(); }

private:
    template <typename T, typename = void> struct IsHandle : std::false_type {};
    template <typename T>
    struct IsHandle<T, std::void_t<typename T::TagType>>
        : std::is_same<T, Handle<typename T::TagType>> {};

    template <typename T> static void appendArg(std::ostringstream &out, const T &value) {
        if constexpr (std::is_same_v<T, bool>) {
            out << (value ? "true" : "false");
        } else if constexpr (std::is_enum_v<T>) {
            out << static_cast<long long>(value);
        } else if constexpr (std::is_integral_v<T>) {
            out << +value;
        } else if constexpr (std::is_floating_point_v<T>) {
            out << value;
        } else if constexpr (std::is_convertible_v<const T &, std::string_view>) {
            out << '"' << std::string_view(value) << '"';
        } else if constexpr (IsHandle<T>::value) {
            out << value.value;
        } else {
            out << "{}";
        }
    }

    std::vector<std::string> calls_;
};

} // namespace sfcop::platform
