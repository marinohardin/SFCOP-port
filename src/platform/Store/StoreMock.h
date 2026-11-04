// A Store for tests: records each call and returns the values the test programs. Read() serves
// bytes from `readData` front first; Write() and WriteAtomic() append to `written`; the registry
// calls use `registry`, a flat map from "key|name" (exactly as passed; the mock does not fold
// case) to value.
#pragma once

#include "platform/Core/MockLog.h"
#include "platform/Store/Store.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace sfcop::platform {

class StoreMock final : public Store {
public:
    MockLog log;

    std::vector<std::uint8_t> readData;
    std::vector<std::uint8_t> written;
    std::map<std::string, RegistryValue> registry;
    RegistryValue lastSetValue;

    Result<std::string> rootPathResult = std::string("/test/root");
    Result<FileHandle> openResult = FileHandle{1};
    Error readError = Error::kNone;  // when set, Read() fails with it
    Error writeError = Error::kNone; // when set, Write() and WriteAtomic() fail with it
    Result<std::uint64_t> seekResult = std::uint64_t{0};
    Result<FileInfo> infoResult = FileInfo{};
    Status setSizeResult;
    Status setModifiedTimeResult;
    Status flushResult;
    Status closeResult;
    Result<FileInfo> statResult = FileInfo{};
    Result<std::vector<DirectoryEntry>> listResult = std::vector<DirectoryEntry>{};
    Status makeDirectoryResult;
    Status removeResult;
    Status renameResult;
    Result<bool> keyExistsResult = true;
    Status createKeyResult;
    Status deleteKeyResult;

    Result<std::string> RootPath(Root root) override {
        log.Record("RootPath", root);
        return rootPathResult;
    }
    Result<FileHandle> Open(Root root, std::string_view path, OpenMode mode) override {
        log.Record("Open", root, path, mode);
        return openResult;
    }
    Result<std::size_t> Read(FileHandle file, void *out, std::size_t bytes) override {
        log.Record("Read", file, bytes);
        if (readError != Error::kNone) {
            return readError;
        }
        std::size_t count = (std::min)(bytes, readData.size());
        if (count > 0) {
            std::memcpy(out, readData.data(), count);
        }
        readData.erase(readData.begin(), readData.begin() + static_cast<std::ptrdiff_t>(count));
        return count;
    }
    Result<std::size_t> Write(FileHandle file, const void *data, std::size_t bytes) override {
        log.Record("Write", file, bytes);
        if (writeError != Error::kNone) {
            return writeError;
        }
        const auto *first = static_cast<const std::uint8_t *>(data);
        written.insert(written.end(), first, first + bytes);
        return bytes;
    }
    Result<std::uint64_t> Seek(FileHandle file, std::int64_t offset, SeekOrigin origin) override {
        log.Record("Seek", file, offset, origin);
        return seekResult;
    }
    Result<FileInfo> GetInfo(FileHandle file) override {
        log.Record("GetInfo", file);
        return infoResult;
    }
    Status SetSize(FileHandle file, std::uint64_t bytes) override {
        log.Record("SetSize", file, bytes);
        return setSizeResult;
    }
    Status SetModifiedTime(FileHandle file, std::int64_t modifiedNs) override {
        log.Record("SetModifiedTime", file, modifiedNs);
        return setModifiedTimeResult;
    }
    Status Flush(FileHandle file) override {
        log.Record("Flush", file);
        return flushResult;
    }
    Status Close(FileHandle file) override {
        log.Record("Close", file);
        return closeResult;
    }
    Result<FileInfo> Stat(Root root, std::string_view path) override {
        log.Record("Stat", root, path);
        return statResult;
    }
    Result<std::vector<DirectoryEntry>> List(Root root, std::string_view path) override {
        log.Record("List", root, path);
        return listResult;
    }
    Status MakeDirectory(Root root, std::string_view path) override {
        log.Record("MakeDirectory", root, path);
        return makeDirectoryResult;
    }
    Status Remove(Root root, std::string_view path) override {
        log.Record("Remove", root, path);
        return removeResult;
    }
    Status Rename(Root root, std::string_view from, std::string_view to) override {
        log.Record("Rename", root, from, to);
        return renameResult;
    }
    Status WriteAtomic(Root root, std::string_view path, const void *data,
                       std::size_t bytes) override {
        log.Record("WriteAtomic", root, path, bytes);
        if (writeError != Error::kNone) {
            return writeError;
        }
        const auto *first = static_cast<const std::uint8_t *>(data);
        written.assign(first, first + bytes);
        return {};
    }
    Result<bool> KeyExists(std::string_view key) override {
        log.Record("KeyExists", key);
        return keyExistsResult;
    }
    Status CreateKey(std::string_view key) override {
        log.Record("CreateKey", key);
        return createKeyResult;
    }
    Status DeleteKey(std::string_view key) override {
        log.Record("DeleteKey", key);
        return deleteKeyResult;
    }
    Result<RegistryValue> GetValue(std::string_view key, std::string_view name) override {
        log.Record("GetValue", key, name);
        auto found = registry.find(std::string(key) + "|" + std::string(name));
        if (found == registry.end()) {
            return Error::kNotFound;
        }
        return found->second;
    }
    Status SetValue(std::string_view key, std::string_view name,
                    const RegistryValue &value) override {
        log.Record("SetValue", key, name, value.type);
        lastSetValue = value;
        registry[std::string(key) + "|" + std::string(name)] = value;
        return {};
    }
};

} // namespace sfcop::platform
