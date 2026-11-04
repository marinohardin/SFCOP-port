#include "app/HostPlatform.h"

#include <cstdlib>
#include <cwchar>

#include <windows.h>

namespace sfcop::app {

namespace {

std::wstring widen(const std::string &utf8) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                           nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                        length);
    return wide;
}

std::string narrow(const wchar_t *wide) {
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) {
        return std::string();
    }
    std::string utf8(static_cast<std::size_t>(length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8.data(), length, nullptr, nullptr);
    return utf8;
}

} // namespace

Platform hostPlatform() { return Platform::kWindows; }

// The wide environment, so that a user folder outside the ANSI code page survives.
EnvLookup hostEnvironment() {
    return [](const std::string &name) -> std::optional<std::string> {
        const wchar_t *value = _wgetenv(widen(name).c_str());
        if (value == nullptr) {
            return std::nullopt;
        }
        return narrow(value);
    };
}

} // namespace sfcop::app
