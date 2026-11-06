#include "hle/kernel/LogsStore.h"

#include <utility>

namespace sfcop::hle::kernel {

using platform::DirectoryEntry;
using platform::Error;
using platform::FileHandle;
using platform::FileInfo;
using platform::RegistryValue;
using platform::Result;
using platform::Root;
using platform::Status;

namespace {

constexpr std::string_view kLogs = platform::userFolderName(platform::UserFolder::kLogs);

bool sameNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); i++) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

LogsStore::LogsStore(std::unique_ptr<platform::Store> user, std::unique_ptr<platform::Store> logs)
    : user_(std::move(user)), logs_(std::move(logs)) {}

bool LogsStore::IsLogsPath(std::string_view path, std::string_view *rest) {
    const std::size_t slash = path.find('/');
    const std::string_view first = path.substr(0, slash);
    if (!sameNoCase(first, kLogs)) {
        return false;
    }
    if (rest != nullptr) {
        *rest = slash == std::string_view::npos ? std::string_view() : path.substr(slash + 1);
    }
    return true;
}

LogsStore::Routed LogsStore::Route(Root root, std::string_view path) const {
    std::string_view rest;
    if (root == Root::kUser && IsLogsPath(path, &rest)) {
        return Routed{logs_.get(), std::string(rest)};
    }
    return Routed{user_.get(), std::string(path)};
}

LogsStore::Inner *LogsStore::Find(FileHandle file) {
    const auto found = open_.find(file.value);
    return found == open_.end() ? nullptr : &found->second;
}

Result<std::string> LogsStore::RootPath(Root root) { return user_->RootPath(root); }

Result<FileHandle> LogsStore::Open(Root root, std::string_view path, platform::OpenMode mode) {
    const Routed to = Route(root, path);
    Result<FileHandle> opened = to.store->Open(root, to.path, mode);
    if (!opened) {
        return opened.Code();
    }
    std::lock_guard<std::mutex> hold(lock_);
    while (next_ == 0 || open_.count(next_) != 0) {
        next_++;
    }
    const std::uint32_t id = next_++;
    open_[id] = Inner{to.store, opened.Value()};
    return FileHandle{id};
}

Result<std::size_t> LogsStore::Read(FileHandle file, void *out, std::size_t bytes) {
    std::lock_guard<std::mutex> hold(lock_);
    Inner *inner = Find(file);
    return inner == nullptr ? Result<std::size_t>(Error::kInvalidHandle)
                            : inner->store->Read(inner->handle, out, bytes);
}

Result<std::size_t> LogsStore::Write(FileHandle file, const void *data, std::size_t bytes) {
    std::lock_guard<std::mutex> hold(lock_);
    Inner *inner = Find(file);
    return inner == nullptr ? Result<std::size_t>(Error::kInvalidHandle)
                            : inner->store->Write(inner->handle, data, bytes);
}

Result<std::uint64_t> LogsStore::Seek(FileHandle file, std::int64_t offset,
                                      platform::SeekOrigin origin) {
    std::lock_guard<std::mutex> hold(lock_);
    Inner *inner = Find(file);
    return inner == nullptr ? Result<std::uint64_t>(Error::kInvalidHandle)
                            : inner->store->Seek(inner->handle, offset, origin);
}

Result<FileInfo> LogsStore::GetInfo(FileHandle file) {
    std::lock_guard<std::mutex> hold(lock_);
    Inner *inner = Find(file);
    return inner == nullptr ? Result<FileInfo>(Error::kInvalidHandle)
                            : inner->store->GetInfo(inner->handle);
}

Status LogsStore::SetSize(FileHandle file, std::uint64_t bytes) {
    std::lock_guard<std::mutex> hold(lock_);
    Inner *inner = Find(file);
    return inner == nullptr ? Status(Error::kInvalidHandle)
                            : inner->store->SetSize(inner->handle, bytes);
}

Status LogsStore::SetModifiedTime(FileHandle file, std::int64_t modifiedNs) {
    std::lock_guard<std::mutex> hold(lock_);
    Inner *inner = Find(file);
    return inner == nullptr ? Status(Error::kInvalidHandle)
                            : inner->store->SetModifiedTime(inner->handle, modifiedNs);
}

Status LogsStore::Flush(FileHandle file) {
    std::lock_guard<std::mutex> hold(lock_);
    Inner *inner = Find(file);
    return inner == nullptr ? Status(Error::kInvalidHandle) : inner->store->Flush(inner->handle);
}

Status LogsStore::Close(FileHandle file) {
    std::lock_guard<std::mutex> hold(lock_);
    Inner *inner = Find(file);
    if (inner == nullptr) {
        return Error::kInvalidHandle;
    }
    const Inner closing = *inner;
    open_.erase(file.value);
    return closing.store->Close(closing.handle);
}

Result<FileInfo> LogsStore::Stat(Root root, std::string_view path) {
    const Routed to = Route(root, path);
    if (to.store == logs_.get() && to.path.empty()) {
        // The logs folder itself: a directory as long as its root is there.
        Result<std::string> there = logs_->RootPath(Root::kUser);
        if (!there) {
            return there.Code();
        }
        FileInfo info;
        info.isDirectory = true;
        return info;
    }
    return to.store->Stat(root, to.path);
}

Result<std::vector<DirectoryEntry>> LogsStore::List(Root root, std::string_view path) {
    const Routed to = Route(root, path);
    return to.store->List(root, to.path);
}

Status LogsStore::MakeDirectory(Root root, std::string_view path) {
    const Routed to = Route(root, path);
    if (to.store == logs_.get() && to.path.empty()) {
        return {}; // the logs root exists
    }
    return to.store->MakeDirectory(root, to.path);
}

Status LogsStore::Remove(Root root, std::string_view path) {
    const Routed to = Route(root, path);
    if (to.store == logs_.get() && to.path.empty()) {
        return Error::kAccessDenied;
    }
    return to.store->Remove(root, to.path);
}

Status LogsStore::Rename(Root root, std::string_view from, std::string_view to) {
    const Routed source = Route(root, from);
    const Routed target = Route(root, to);
    if (source.store != target.store || source.path.empty() || target.path.empty()) {
        return Error::kAccessDenied;
    }
    return source.store->Rename(root, source.path, target.path);
}

Status LogsStore::WriteAtomic(Root root, std::string_view path, const void *data,
                              std::size_t bytes) {
    const Routed to = Route(root, path);
    if (to.store == logs_.get() && to.path.empty()) {
        return Error::kInvalidArgument;
    }
    return to.store->WriteAtomic(root, to.path, data, bytes);
}

Result<bool> LogsStore::KeyExists(std::string_view key) { return user_->KeyExists(key); }
Status LogsStore::CreateKey(std::string_view key) { return user_->CreateKey(key); }
Status LogsStore::DeleteKey(std::string_view key) { return user_->DeleteKey(key); }
Result<RegistryValue> LogsStore::GetValue(std::string_view key, std::string_view name) {
    return user_->GetValue(key, name);
}
Status LogsStore::SetValue(std::string_view key, std::string_view name,
                           const RegistryValue &value) {
    return user_->SetValue(key, name, value);
}

} // namespace sfcop::hle::kernel
