// ADVAPI32's registry and event-log calls, on Store's key store (DESIGN §5.3, §6.6). Plain C++:
// RegistryGuest.c reads the game's arguments and calls this through RegistryBridge.h.
//
// A key is named as the game names it, the predefined key's name first:
// `HKEY_LOCAL_MACHINE\SOFTWARE\...`. Key and value names are the game's bytes; Store compares
// them without ASCII case, as Windows does. The Reg* calls answer a Win32 error code (0 for
// success); the event-log calls answer BOOL or a handle and set the last error, which
// TakeLastError() hands to the shim.
#pragma once

#include "platform/Store/Store.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace sfcop::hle::registry {

namespace win32 {
constexpr std::uint32_t kErrorSuccess = 0;
constexpr std::uint32_t kErrorFileNotFound = 2;
constexpr std::uint32_t kErrorAccessDenied = 5;
constexpr std::uint32_t kErrorInvalidHandle = 6;
constexpr std::uint32_t kErrorNotEnoughMemory = 8;
constexpr std::uint32_t kErrorGenFailure = 31;
constexpr std::uint32_t kErrorInvalidParameter = 87;
constexpr std::uint32_t kErrorBadPathname = 161;
constexpr std::uint32_t kErrorMoreData = 234;
constexpr std::uint32_t kErrorRegistryIoFailed = 1016;

constexpr std::uint32_t kClassesRoot = 0x80000000u;
constexpr std::uint32_t kCurrentUser = 0x80000001u;
constexpr std::uint32_t kLocalMachine = 0x80000002u;
constexpr std::uint32_t kUsers = 0x80000003u;
constexpr std::uint32_t kPerformanceData = 0x80000004u;
constexpr std::uint32_t kCurrentConfig = 0x80000005u;
constexpr std::uint32_t kDynData = 0x80000006u;

constexpr std::uint32_t kRegNone = 0;
constexpr std::uint32_t kRegSz = 1;
constexpr std::uint32_t kRegBinary = 3;
constexpr std::uint32_t kRegDword = 4;

constexpr std::uint32_t kCreatedNewKey = 1;
constexpr std::uint32_t kOpenedExistingKey = 2;
} // namespace win32

class Registry {
public:
    explicit Registry(platform::Store &store) : store_(store) {}

    // RegOpenKeyExA: `subKey` null or empty opens a new handle to `parent` itself.
    std::uint32_t OpenKey(std::uint32_t parent, const char *subKey, std::uint32_t *result);
    // RegCreateKeyExA: creates the key and its missing parents; `disposition` may be null.
    std::uint32_t CreateKey(std::uint32_t parent, const char *subKey, std::uint32_t *result,
                            std::uint32_t *disposition);
    std::uint32_t CloseKey(std::uint32_t key);
    // RegDeleteKeyA: the key and everything below it (Store deletes a whole subtree).
    std::uint32_t DeleteKey(std::uint32_t parent, const char *subKey);
    // RegQueryValueExA: the type, and the data when `data` is given. `size` is the buffer's size
    // in, the data's size out; ERROR_MORE_DATA when the buffer is too small (nothing written).
    // A REG_SZ's data and size include its NUL.
    std::uint32_t QueryValue(std::uint32_t key, const char *name, std::uint32_t *type,
                             std::uint8_t *data, std::uint32_t *size);
    // RegSetValueExA for REG_DWORD (4 bytes), REG_SZ (up to its NUL) and REG_BINARY, the types
    // Store keeps; any other type is ERROR_INVALID_PARAMETER.
    std::uint32_t SetValue(std::uint32_t key, const char *name, std::uint32_t type,
                           const std::uint8_t *data, std::uint32_t size);

    // The event log: a source can be opened and closed and a report is accepted, but nothing is
    // recorded (a report could carry what DESIGN §9 keeps out of logs).
    std::uint32_t RegisterEventSource(const char *server, const char *source);
    bool DeregisterEventSource(std::uint32_t handle);
    bool ReportEvent(std::uint32_t handle);

    std::optional<std::uint32_t> TakeLastError();
    // The full name of an open or predefined key.
    std::optional<std::string> KeyName(std::uint32_t key) const;

private:
    // The full name of `subKey` under `parent`, or the error that prevents one.
    std::uint32_t Join(std::uint32_t parent, const char *subKey, std::string &name) const;
    std::uint32_t NewHandle(std::string name);

    platform::Store &store_;
    std::map<std::uint32_t, std::string> keys_;
    std::map<std::uint32_t, std::string> eventSources_;
    std::uint32_t nextHandle_ = 0x1000;
    std::optional<std::uint32_t> lastError_;
};

} // namespace sfcop::hle::registry
