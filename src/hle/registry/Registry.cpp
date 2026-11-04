#include "hle/registry/Registry.h"

#include <cstring>
#include <vector>

namespace sfcop::hle::registry {
namespace {

using platform::Error;
using platform::RegistryType;
using platform::RegistryValue;

const char *predefinedName(std::uint32_t key) {
    switch (key) {
    case win32::kClassesRoot:
        return "HKEY_CLASSES_ROOT";
    case win32::kCurrentUser:
        return "HKEY_CURRENT_USER";
    case win32::kLocalMachine:
        return "HKEY_LOCAL_MACHINE";
    case win32::kUsers:
        return "HKEY_USERS";
    case win32::kPerformanceData:
        return "HKEY_PERFORMANCE_DATA";
    case win32::kCurrentConfig:
        return "HKEY_CURRENT_CONFIG";
    case win32::kDynData:
        return "HKEY_DYN_DATA";
    default:
        return nullptr;
    }
}

std::uint32_t errorOf(Error error) {
    switch (error) {
    case Error::kNotFound:
        return win32::kErrorFileNotFound;
    case Error::kInvalidArgument:
        return win32::kErrorInvalidParameter;
    case Error::kAccessDenied:
        return win32::kErrorAccessDenied;
    case Error::kOutOfResources:
        return win32::kErrorNotEnoughMemory;
    default:
        return win32::kErrorRegistryIoFailed;
    }
}

} // namespace

std::optional<std::string> Registry::KeyName(std::uint32_t key) const {
    if (const char *name = predefinedName(key)) {
        return std::string(name);
    }
    auto found = keys_.find(key);
    if (found == keys_.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::uint32_t Registry::Join(std::uint32_t parent, const char *subKey, std::string &name) const {
    std::optional<std::string> base = KeyName(parent);
    if (!base) {
        return win32::kErrorInvalidHandle;
    }
    name = *base;
    std::string sub = subKey != nullptr ? subKey : "";
    if (!sub.empty() && sub.back() == '\\') {
        sub.pop_back();
    }
    if (sub.empty()) {
        return win32::kErrorSuccess;
    }
    if (sub.front() == '\\' || sub.find("\\\\") != std::string::npos) {
        return win32::kErrorBadPathname;
    }
    name += "\\" + sub;
    return win32::kErrorSuccess;
}

std::uint32_t Registry::NewHandle(std::string name) {
    std::uint32_t handle = nextHandle_;
    nextHandle_ += 4;
    keys_[handle] = std::move(name);
    return handle;
}

std::uint32_t Registry::OpenKey(std::uint32_t parent, const char *subKey, std::uint32_t *result) {
    if (result == nullptr) {
        return win32::kErrorInvalidParameter;
    }
    *result = 0;
    std::string name;
    std::uint32_t error = Join(parent, subKey, name);
    if (error != win32::kErrorSuccess) {
        return error;
    }
    platform::Result<bool> exists = store_.KeyExists(name);
    if (!exists) {
        return errorOf(exists.Code());
    }
    if (!exists.Value()) {
        return win32::kErrorFileNotFound;
    }
    *result = NewHandle(name);
    return win32::kErrorSuccess;
}

std::uint32_t Registry::CreateKey(std::uint32_t parent, const char *subKey, std::uint32_t *result,
                                  std::uint32_t *disposition) {
    if (result == nullptr) {
        return win32::kErrorInvalidParameter;
    }
    *result = 0;
    std::string name;
    std::uint32_t error = Join(parent, subKey, name);
    if (error != win32::kErrorSuccess) {
        return error;
    }
    platform::Result<bool> exists = store_.KeyExists(name);
    if (!exists) {
        return errorOf(exists.Code());
    }
    if (!exists.Value()) {
        platform::Status created = store_.CreateKey(name);
        if (!created) {
            return errorOf(created.Code());
        }
    }
    if (disposition != nullptr) {
        *disposition = exists.Value() ? win32::kOpenedExistingKey : win32::kCreatedNewKey;
    }
    *result = NewHandle(name);
    return win32::kErrorSuccess;
}

std::uint32_t Registry::CloseKey(std::uint32_t key) {
    if (predefinedName(key) != nullptr) {
        return win32::kErrorSuccess;
    }
    return keys_.erase(key) > 0 ? win32::kErrorSuccess : win32::kErrorInvalidHandle;
}

std::uint32_t Registry::DeleteKey(std::uint32_t parent, const char *subKey) {
    if (subKey == nullptr) {
        return win32::kErrorInvalidParameter;
    }
    std::string name;
    std::uint32_t error = Join(parent, subKey, name);
    if (error != win32::kErrorSuccess) {
        return error;
    }
    if (name.find('\\') == std::string::npos) {
        return win32::kErrorAccessDenied; // a predefined key itself
    }
    platform::Status deleted = store_.DeleteKey(name);
    return deleted ? win32::kErrorSuccess : errorOf(deleted.Code());
}

std::uint32_t Registry::QueryValue(std::uint32_t key, const char *name, std::uint32_t *type,
                                   std::uint8_t *data, std::uint32_t *size) {
    std::optional<std::string> keyName = KeyName(key);
    if (!keyName) {
        return win32::kErrorInvalidHandle;
    }
    if (data != nullptr && size == nullptr) {
        return win32::kErrorInvalidParameter;
    }
    platform::Result<RegistryValue> value =
        store_.GetValue(*keyName, name != nullptr ? name : "");
    if (!value) {
        return errorOf(value.Code());
    }
    std::vector<std::uint8_t> bytes;
    std::uint32_t valueType = win32::kRegBinary;
    switch (value.Value().type) {
    case RegistryType::kDword: {
        std::uint32_t number = value.Value().dword;
        bytes = {static_cast<std::uint8_t>(number), static_cast<std::uint8_t>(number >> 8),
                 static_cast<std::uint8_t>(number >> 16), static_cast<std::uint8_t>(number >> 24)};
        valueType = win32::kRegDword;
        break;
    }
    case RegistryType::kString:
        bytes.assign(value.Value().text.begin(), value.Value().text.end());
        bytes.push_back(0);
        valueType = win32::kRegSz;
        break;
    case RegistryType::kBinary:
        bytes = value.Value().bytes;
        break;
    }
    if (type != nullptr) {
        *type = valueType;
    }
    if (size == nullptr) {
        return win32::kErrorSuccess;
    }
    auto needed = static_cast<std::uint32_t>(bytes.size());
    if (data != nullptr && *size < needed) {
        *size = needed;
        return win32::kErrorMoreData;
    }
    if (data != nullptr && needed > 0) {
        std::memcpy(data, bytes.data(), needed);
    }
    *size = needed;
    return win32::kErrorSuccess;
}

std::uint32_t Registry::SetValue(std::uint32_t key, const char *name, std::uint32_t type,
                                 const std::uint8_t *data, std::uint32_t size) {
    std::optional<std::string> keyName = KeyName(key);
    if (!keyName) {
        return win32::kErrorInvalidHandle;
    }
    if (data == nullptr && size > 0) {
        return win32::kErrorInvalidParameter;
    }
    RegistryValue value;
    if (type == win32::kRegDword) {
        if (size != 4) {
            return win32::kErrorInvalidParameter;
        }
        value.type = RegistryType::kDword;
        value.dword = static_cast<std::uint32_t>(data[0]) | static_cast<std::uint32_t>(data[1]) << 8 |
                      static_cast<std::uint32_t>(data[2]) << 16 |
                      static_cast<std::uint32_t>(data[3]) << 24;
    } else if (type == win32::kRegSz) {
        value.type = RegistryType::kString;
        const char *text = reinterpret_cast<const char *>(data);
        std::size_t length = 0;
        while (length < size && text[length] != '\0') {
            length++;
        }
        value.text.assign(text != nullptr ? text : "", length);
    } else if (type == win32::kRegBinary) {
        value.type = RegistryType::kBinary;
        if (size > 0) {
            value.bytes.assign(data, data + size);
        }
    } else {
        return win32::kErrorInvalidParameter;
    }
    platform::Status set = store_.SetValue(*keyName, name != nullptr ? name : "", value);
    return set ? win32::kErrorSuccess : errorOf(set.Code());
}

std::uint32_t Registry::RegisterEventSource(const char *server, const char *source) {
    (void)server;
    std::uint32_t handle = nextHandle_;
    nextHandle_ += 4;
    eventSources_[handle] = source != nullptr ? source : "";
    return handle;
}

bool Registry::DeregisterEventSource(std::uint32_t handle) {
    if (eventSources_.erase(handle) == 0) {
        lastError_ = win32::kErrorInvalidHandle;
        return false;
    }
    return true;
}

bool Registry::ReportEvent(std::uint32_t handle) {
    if (eventSources_.count(handle) == 0) {
        lastError_ = win32::kErrorInvalidHandle;
        return false;
    }
    return true;
}

std::optional<std::uint32_t> Registry::TakeLastError() {
    std::optional<std::uint32_t> error = lastError_;
    lastError_.reset();
    return error;
}

} // namespace sfcop::hle::registry
