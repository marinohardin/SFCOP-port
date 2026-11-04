// For the kernel and registry modules' tests: a Store that forwards every call to another and
// lets a test look at it first. RecordingStore keeps the sequence and flags any call that would
// change the install; ThrowingStore throws on the first call that reaches it.
#pragma once

#include "platform/Store/Store.h"

#include <cstdint>
#include <memory>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace sfcop::hle::kernel::test {

class ForwardingStore : public platform::Store {
public:
    explicit ForwardingStore(std::unique_ptr<platform::Store> inner) : inner_(std::move(inner)) {}

    platform::Result<std::string> RootPath(platform::Root root) override {
        Before("RootPath", &root, nullptr, {});
        return inner_->RootPath(root);
    }
    platform::Result<platform::FileHandle> Open(platform::Root root, std::string_view path,
                                                platform::OpenMode mode) override {
        Before("Open", &root, &mode, {});
        platform::Result<platform::FileHandle> opened = inner_->Open(root, path, mode);
        if (opened) {
            Opened(opened.Value(), root);
        }
        return opened;
    }
    platform::Result<std::size_t> Read(platform::FileHandle file, void *out,
                                       std::size_t bytes) override {
        Before("Read", nullptr, nullptr, file);
        return inner_->Read(file, out, bytes);
    }
    platform::Result<std::size_t> Write(platform::FileHandle file, const void *data,
                                        std::size_t bytes) override {
        Before("Write", nullptr, nullptr, file);
        return inner_->Write(file, data, bytes);
    }
    platform::Result<std::uint64_t> Seek(platform::FileHandle file, std::int64_t offset,
                                         platform::SeekOrigin origin) override {
        Before("Seek", nullptr, nullptr, file);
        return inner_->Seek(file, offset, origin);
    }
    platform::Result<platform::FileInfo> GetInfo(platform::FileHandle file) override {
        Before("GetInfo", nullptr, nullptr, file);
        return inner_->GetInfo(file);
    }
    platform::Status SetSize(platform::FileHandle file, std::uint64_t bytes) override {
        Before("SetSize", nullptr, nullptr, file);
        return inner_->SetSize(file, bytes);
    }
    platform::Status SetModifiedTime(platform::FileHandle file, std::int64_t modifiedNs) override {
        Before("SetModifiedTime", nullptr, nullptr, file);
        return inner_->SetModifiedTime(file, modifiedNs);
    }
    platform::Status Flush(platform::FileHandle file) override {
        Before("Flush", nullptr, nullptr, file);
        return inner_->Flush(file);
    }
    platform::Status Close(platform::FileHandle file) override {
        Before("Close", nullptr, nullptr, file);
        return inner_->Close(file);
    }
    platform::Result<platform::FileInfo> Stat(platform::Root root, std::string_view path) override {
        Before("Stat", &root, nullptr, {});
        return inner_->Stat(root, path);
    }
    platform::Result<std::vector<platform::DirectoryEntry>> List(platform::Root root,
                                                                 std::string_view path) override {
        Before("List", &root, nullptr, {});
        return inner_->List(root, path);
    }
    platform::Status MakeDirectory(platform::Root root, std::string_view path) override {
        Before("MakeDirectory", &root, nullptr, {});
        return inner_->MakeDirectory(root, path);
    }
    platform::Status Remove(platform::Root root, std::string_view path) override {
        Before("Remove", &root, nullptr, {});
        return inner_->Remove(root, path);
    }
    platform::Status Rename(platform::Root root, std::string_view from,
                            std::string_view to) override {
        Before("Rename", &root, nullptr, {});
        return inner_->Rename(root, from, to);
    }
    platform::Status WriteAtomic(platform::Root root, std::string_view path, const void *data,
                                 std::size_t bytes) override {
        Before("WriteAtomic", &root, nullptr, {});
        return inner_->WriteAtomic(root, path, data, bytes);
    }
    platform::Result<bool> KeyExists(std::string_view key) override {
        Before("KeyExists", nullptr, nullptr, {});
        return inner_->KeyExists(key);
    }
    platform::Status CreateKey(std::string_view key) override {
        Before("CreateKey", nullptr, nullptr, {});
        return inner_->CreateKey(key);
    }
    platform::Status DeleteKey(std::string_view key) override {
        Before("DeleteKey", nullptr, nullptr, {});
        return inner_->DeleteKey(key);
    }
    platform::Result<platform::RegistryValue> GetValue(std::string_view key,
                                                       std::string_view name) override {
        Before("GetValue", nullptr, nullptr, {});
        return inner_->GetValue(key, name);
    }
    platform::Status SetValue(std::string_view key, std::string_view name,
                              const platform::RegistryValue &value) override {
        Before("SetValue", nullptr, nullptr, {});
        return inner_->SetValue(key, name, value);
    }

protected:
    // Called before the call is forwarded; `root` and `mode` are null where the call has none.
    virtual void Before(const char *call, const platform::Root *root, const platform::OpenMode *mode,
                        platform::FileHandle file) = 0;
    // A file the inner Store opened.
    virtual void Opened(platform::FileHandle, platform::Root) {}

private:
    std::unique_ptr<platform::Store> inner_;
};

// Records each call as `Name root mode` and notes a violation for any call that would write the
// install: an Open of an install file for more than reading, a MakeDirectory, Remove, Rename or
// WriteAtomic under it, or a change through a handle that was opened on it.
class RecordingStore final : public ForwardingStore {
public:
    using ForwardingStore::ForwardingStore;

    std::vector<std::string> calls;      // "Stat install", "Open user 2", "Write", ...
    std::vector<std::string> violations; // the calls that would write the install
    std::size_t installCalls = 0;        // every call that carried the install root

protected:
    void Before(const char *call, const platform::Root *root, const platform::OpenMode *mode,
                platform::FileHandle file) override {
        std::string line = call;
        const std::string name = call;
        const bool installRoot = root != nullptr && *root == platform::Root::kInstall;
        if (root != nullptr) {
            line += installRoot ? " install" : " user";
        }
        if (mode != nullptr) {
            line += " " + std::to_string(static_cast<int>(*mode));
        }
        calls.push_back(line);
        if (installRoot) {
            installCalls++;
        }
        const bool mutating =
            name == "MakeDirectory" || name == "Remove" || name == "Rename" ||
            name == "WriteAtomic" ||
            (name == "Open" && mode != nullptr && *mode != platform::OpenMode::kRead);
        if (installRoot && mutating) {
            violations.push_back(line);
        }
        const bool changesFile = name == "Write" || name == "SetSize" || name == "SetModifiedTime";
        if (changesFile && installHandles_.count(file.value) > 0) {
            violations.push_back(line + " on an install handle");
        }
    }
    void Opened(platform::FileHandle file, platform::Root root) override {
        if (root == platform::Root::kInstall) {
            installHandles_.insert(file.value);
        } else {
            installHandles_.erase(file.value);
        }
    }

private:
    std::set<std::uint32_t> installHandles_;
};

// Throws on the call that follows `passThrough` calls that went through: std::bad_alloc, or
// std::runtime_error (what std::filesystem throws).
class ThrowingStore final : public ForwardingStore {
public:
    ThrowingStore(std::unique_ptr<platform::Store> inner, bool allocation)
        : ForwardingStore(std::move(inner)), allocation_(allocation) {}

    std::size_t passThrough = 0;

protected:
    void Before(const char *, const platform::Root *, const platform::OpenMode *,
                platform::FileHandle) override {
        if (made_++ < passThrough) {
            return;
        }
        if (allocation_) {
            throw std::bad_alloc();
        }
        throw std::runtime_error("injected failure");
    }

private:
    bool allocation_;
    std::size_t made_ = 0;
};

} // namespace sfcop::hle::kernel::test
