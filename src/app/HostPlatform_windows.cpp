#include "app/HostPlatform.h"

#include <cstdlib>
#include <cwchar>
#include <vector>

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

std::string executablePath() {
    std::vector<wchar_t> path(MAX_PATH);
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return std::string();
        }
        if (length < path.size()) {
            return narrow(path.data());
        }
        path.resize(path.size() * 2);
    }
}

} // namespace sfcop::app
