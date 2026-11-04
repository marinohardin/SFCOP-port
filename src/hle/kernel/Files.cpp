// CreateFile and the calls on an open file; deleting, moving, copying and the attributes of a
// path; directories. Every write lands in the per-user copy (Kernel.h).

#include "hle/kernel/Kernel.h"

#include <vector>

namespace sfcop::hle::kernel {
namespace {

using platform::Error;
using platform::FileHandle;
using platform::OpenMode;
using platform::Result;
using platform::Root;
using platform::SeekOrigin;
using platform::Status;

constexpr std::uint32_t kWriteAccess =
    win32::kGenericWrite | win32::kGenericAll | win32::kFileWriteData | win32::kFileAppendData;
constexpr std::uint32_t kReadAccess = win32::kGenericRead | win32::kGenericAll | win32::kFileReadData;

std::uint32_t attributesOf(const platform::FileInfo &info) {
    if (info.isDirectory) {
        return win32::kAttributeDirectory;
    }
    return win32::kAttributeArchive | (info.readOnly ? win32::kAttributeReadOnly : 0u);
}

} // namespace

std::optional<std::string> Kernel::ReadWhole(const Entry &entry) {
    Result<FileHandle> file = store_.Open(entry.root, entry.path, OpenMode::kRead);
    if (!file) {
        SetError(win32Error(file.Code(), false));
        return std::nullopt;
    }
    std::string content;
    char chunk[65536];
    for (;;) {
        Result<std::size_t> got = store_.Read(file.Value(), chunk, sizeof chunk);
        if (!got) {
            (void)store_.Close(file.Value());
            SetError(win32Error(got.Code(), false));
            return std::nullopt;
        }
        if (got.Value() == 0) {
            break;
        }
        content.append(chunk, got.Value());
    }
    (void)store_.Close(file.Value());
    return content;
}

bool Kernel::CopyToUser(const Entry &from, std::string_view userPath) {
    std::optional<std::string> content = ReadWhole(from);
    if (!content) {
        return false; // ReadWhole set the read error
    }
    Status made = MakeUserParent(userPath);
    if (!made) {
        SetError(win32Error(made.Code(), true));
        return false;
    }
    Result<FileHandle> to = store_.Open(Root::kUser, userPath, OpenMode::kCreate);
    if (!to) {
        SetError(win32Error(to.Code(), true));
        return false;
    }
    Result<std::size_t> put = content->empty() ? Result<std::size_t>(0)
                                               : store_.Write(to.Value(), content->data(),
                                                              content->size());
    if (put) {
        (void)store_.SetModifiedTime(to.Value(), from.info.modifiedNs);
    }
    Status closed = store_.Close(to.Value());
    if (!put) {
        SetError(win32Error(put.Code(), true));
        return false;
    }
    if (!closed) {
        SetError(win32Error(closed.Code(), true));
        return false;
    }
    return true;
}

std::uint32_t Kernel::CreateFile(std::string_view name, std::uint32_t access, std::uint32_t share,
                                 std::uint32_t disposition, std::uint32_t flags) {
    (void)share; // the game's own sharing never conflicts with itself; Store has none to ask
    (void)flags;
    if (disposition < win32::kCreateNew || disposition > win32::kTruncateExisting ||
        (disposition == win32::kTruncateExisting && (access & kWriteAccess) == 0)) {
        SetError(win32::kErrorInvalidParameter);
        return 0;
    }
    GamePath where = Locate(name);
    if (!where.inGame) {
        SetError(Absolute(name).empty() && !name.empty() ? win32::kErrorInvalidName
                                                          : win32::kErrorPathNotFound);
        return 0;
    }
    std::optional<Entry> existing = Lookup(where.relative);
    if (existing && existing->info.isDirectory) {
        SetError(win32::kErrorAccessDenied);
        return 0;
    }
    if (!existing && (disposition == win32::kOpenExisting ||
                      disposition == win32::kTruncateExisting)) {
        SetError(MissingError(where.relative));
        return 0;
    }
    if (existing && disposition == win32::kCreateNew) {
        SetError(win32::kErrorFileExists);
        return 0;
    }

    const bool wantsWrite = (access & kWriteAccess) != 0;
    const bool creates = !existing || disposition == win32::kCreateAlways ||
                         disposition == win32::kTruncateExisting;
    Result<FileHandle> handle = Error::kNotFound;
    if (!wantsWrite && !creates) {
        handle = store_.Open(existing->root, existing->path, OpenMode::kRead);
    } else {
        std::string user = UserPathOf(where.relative);
        if (!existing) {
            std::size_t slash = where.relative.rfind('/');
            if (!IsDirectory(slash == std::string::npos ? "" : where.relative.substr(0, slash))) {
                SetError(win32::kErrorPathNotFound);
                return 0;
            }
        }
        Status ready = MakeUserParent(user);
        if (!ready) {
            SetError(win32Error(ready.Code(), true));
            return 0;
        }
        if (!creates && existing->root == Root::kInstall && !CopyToUser(*existing, user)) {
            return 0;
        }
        handle = store_.Open(Root::kUser, user, creates ? OpenMode::kCreate : OpenMode::kReadWrite);
    }
    if (!handle) {
        SetError(win32Error(handle.Code(), wantsWrite));
        return 0;
    }
    std::uint32_t id = nextId_++;
    files_[id] = OpenFile{handle.Value(), (access & kReadAccess) != 0 || !wantsWrite, wantsWrite};
    SetError(existing && (disposition == win32::kCreateAlways || disposition == win32::kOpenAlways)
                 ? win32::kErrorAlreadyExists
                 : 0u);
    return id;
}

bool Kernel::ReadFile(std::uint32_t file, void *out, std::uint32_t bytes, std::uint32_t *read) {
    if (read != nullptr) {
        *read = 0;
    }
    auto open = files_.find(file);
    if (open == files_.end()) {
        SetError(win32::kErrorInvalidHandle);
        return false;
    }
    if (!open->second.canRead) {
        SetError(win32::kErrorAccessDenied);
        return false;
    }
    std::uint32_t done = 0;
    while (done < bytes) {
        Result<std::size_t> got =
            store_.Read(open->second.handle, static_cast<char *>(out) + done, bytes - done);
        if (!got) {
            if (read != nullptr) {
                *read = done;
            }
            SetError(win32Error(got.Code(), false));
            return false;
        }
        if (got.Value() == 0) {
            break;
        }
        done += static_cast<std::uint32_t>(got.Value());
    }
    if (read != nullptr) {
        *read = done;
    }
    return true; // the end of the file is success with fewer bytes
}

bool Kernel::WriteFile(std::uint32_t file, const void *data, std::uint32_t bytes,
                       std::uint32_t *written) {
    if (written != nullptr) {
        *written = 0;
    }
    auto open = files_.find(file);
    if (open == files_.end()) {
        SetError(win32::kErrorInvalidHandle);
        return false;
    }
    if (!open->second.canWrite) {
        SetError(win32::kErrorAccessDenied);
        return false;
    }
    if (bytes == 0) {
        return true;
    }
    Result<std::size_t> put = store_.Write(open->second.handle, data, bytes);
    if (!put) {
        SetError(win32Error(put.Code(), true));
        return false;
    }
    if (written != nullptr) {
        *written = static_cast<std::uint32_t>(put.Value());
    }
    return true;
}

std::uint32_t Kernel::SetFilePointer(std::uint32_t file, std::int32_t distance, std::int32_t *high,
                                     std::uint32_t method) {
    auto open = files_.find(file);
    if (open == files_.end()) {
        SetError(win32::kErrorInvalidHandle);
        return win32::kInvalidSetFilePointer;
    }
    if (method > win32::kFileEnd) {
        SetError(win32::kErrorInvalidParameter);
        return win32::kInvalidSetFilePointer;
    }
    std::int64_t offset = distance;
    if (high != nullptr) {
        offset = static_cast<std::int64_t>(static_cast<std::uint64_t>(
                                               static_cast<std::uint32_t>(*high))
                                               << 32 |
                                           static_cast<std::uint32_t>(distance));
    }
    SeekOrigin origin = method == win32::kFileBegin     ? SeekOrigin::kBegin
                        : method == win32::kFileCurrent ? SeekOrigin::kCurrent
                                                        : SeekOrigin::kEnd;
    Result<std::uint64_t> position = store_.Seek(open->second.handle, offset, origin);
    if (!position) {
        SetError(position.Code() == Error::kInvalidArgument ? win32::kErrorNegativeSeek
                                                            : win32Error(position.Code(), false));
        return win32::kInvalidSetFilePointer;
    }
    if (high != nullptr) {
        *high = static_cast<std::int32_t>(position.Value() >> 32);
    }
    SetError(0); // so a position of 0xFFFFFFFF reads as success
    return static_cast<std::uint32_t>(position.Value());
}

std::uint32_t Kernel::GetFileSize(std::uint32_t file, std::uint32_t *high) {
    auto open = files_.find(file);
    if (open == files_.end()) {
        SetError(win32::kErrorInvalidHandle);
        return win32::kInvalidFileSize;
    }
    Result<platform::FileInfo> info = store_.GetInfo(open->second.handle);
    if (!info) {
        SetError(win32Error(info.Code(), false));
        return win32::kInvalidFileSize;
    }
    if (high != nullptr) {
        *high = static_cast<std::uint32_t>(info.Value().size >> 32);
    }
    SetError(0);
    return static_cast<std::uint32_t>(info.Value().size);
}

bool Kernel::GetFileTime(std::uint32_t file, std::uint64_t *creation, std::uint64_t *access,
                         std::uint64_t *write) {
    auto open = files_.find(file);
    if (open == files_.end()) {
        SetError(win32::kErrorInvalidHandle);
        return false;
    }
    Result<platform::FileInfo> info = store_.GetInfo(open->second.handle);
    if (!info) {
        SetError(win32Error(info.Code(), false));
        return false;
    }
    std::uint64_t time = fileTimeFromNs(info.Value().modifiedNs);
    for (std::uint64_t *out : {creation, access, write}) {
        if (out != nullptr) {
            *out = time;
        }
    }
    return true;
}

bool Kernel::SetFileTime(std::uint32_t file, const std::uint64_t *creation,
                         const std::uint64_t *access, const std::uint64_t *write) {
    (void)creation; // Store keeps the modification time only
    (void)access;
    auto open = files_.find(file);
    if (open == files_.end()) {
        SetError(win32::kErrorInvalidHandle);
        return false;
    }
    if (!open->second.canWrite) {
        SetError(win32::kErrorAccessDenied);
        return false;
    }
    if (write == nullptr) {
        return true;
    }
    Status set = store_.SetModifiedTime(open->second.handle, nsFromFileTime(*write));
    if (!set) {
        SetError(win32Error(set.Code(), true));
        return false;
    }
    return true;
}

bool Kernel::FlushFileBuffers(std::uint32_t file) {
    auto open = files_.find(file);
    if (open == files_.end()) {
        SetError(win32::kErrorInvalidHandle);
        return false;
    }
    if (!open->second.canWrite) {
        SetError(win32::kErrorAccessDenied);
        return false;
    }
    Status flushed = store_.Flush(open->second.handle);
    if (!flushed) {
        SetError(win32Error(flushed.Code(), true));
        return false;
    }
    return true;
}

bool Kernel::SetEndOfFile(std::uint32_t file) {
    auto open = files_.find(file);
    if (open == files_.end()) {
        SetError(win32::kErrorInvalidHandle);
        return false;
    }
    if (!open->second.canWrite) {
        SetError(win32::kErrorAccessDenied);
        return false;
    }
    Result<std::uint64_t> position = store_.Seek(open->second.handle, 0, SeekOrigin::kCurrent);
    Status sized = position ? store_.SetSize(open->second.handle, position.Value())
                            : Status{position.Code()};
    if (!sized) {
        SetError(win32Error(sized.Code(), true));
        return false;
    }
    return true;
}

bool Kernel::CloseFile(std::uint32_t file) {
    auto open = files_.find(file);
    if (open == files_.end()) {
        SetError(win32::kErrorInvalidHandle);
        return false;
    }
    FileHandle handle = open->second.handle;
    files_.erase(open);
    Status closed = store_.Close(handle);
    if (!closed) {
        SetError(win32Error(closed.Code(), true));
        return false;
    }
    return true;
}

bool Kernel::DeleteFile(std::string_view name) {
    GamePath where = Locate(name);
    if (!where.inGame) {
        SetError(win32::kErrorPathNotFound);
        return false;
    }
    std::optional<Entry> entry = Lookup(where.relative);
    if (!entry) {
        SetError(MissingError(where.relative));
        return false;
    }
    if (entry->info.isDirectory || entry->root == Root::kInstall) {
        SetError(win32::kErrorAccessDenied);
        return false;
    }
    Status removed = store_.Remove(Root::kUser, entry->path);
    if (!removed) {
        SetError(win32Error(removed.Code(), true));
        return false;
    }
    return true;
}

bool Kernel::MoveFile(std::string_view from, std::string_view to) {
    GamePath source = Locate(from);
    GamePath target = Locate(to);
    if (!source.inGame || !target.inGame) {
        SetError(win32::kErrorPathNotFound);
        return false;
    }
    std::optional<Entry> entry = Lookup(source.relative);
    if (!entry) {
        SetError(MissingError(source.relative));
        return false;
    }
    if (Lookup(target.relative)) {
        SetError(win32::kErrorAlreadyExists);
        return false;
    }
    if (entry->root == Root::kInstall) {
        SetError(win32::kErrorAccessDenied);
        return false;
    }
    std::size_t slash = target.relative.rfind('/');
    if (!IsDirectory(slash == std::string::npos ? "" : target.relative.substr(0, slash))) {
        SetError(win32::kErrorPathNotFound); // as on Windows, the target's folder must exist
        return false;
    }
    std::string user = UserPathOf(target.relative);
    Status moved = MakeUserParent(user);
    if (moved) {
        moved = store_.Rename(Root::kUser, entry->path, user);
    }
    if (!moved) {
        SetError(win32Error(moved.Code(), true));
        return false;
    }
    return true;
}

bool Kernel::CopyFile(std::string_view from, std::string_view to, bool failIfExists) {
    GamePath source = Locate(from);
    GamePath target = Locate(to);
    if (!source.inGame || !target.inGame) {
        SetError(win32::kErrorPathNotFound);
        return false;
    }
    std::optional<Entry> entry = Lookup(source.relative);
    if (!entry || entry->info.isDirectory) {
        SetError(entry ? win32::kErrorAccessDenied : MissingError(source.relative));
        return false;
    }
    std::optional<Entry> existing = Lookup(target.relative);
    if (existing && (failIfExists || existing->info.isDirectory)) {
        SetError(existing->info.isDirectory ? win32::kErrorAccessDenied : win32::kErrorFileExists);
        return false;
    }
    return CopyToUser(*entry, UserPathOf(target.relative));
}

bool Kernel::CreateDirectory(std::string_view name) {
    GamePath where = Locate(name);
    if (!where.inGame) {
        SetError(win32::kErrorPathNotFound);
        return false;
    }
    if (where.relative.empty() || Lookup(where.relative)) {
        SetError(win32::kErrorAlreadyExists);
        return false;
    }
    std::size_t slash = where.relative.rfind('/');
    if (!IsDirectory(slash == std::string::npos ? "" : where.relative.substr(0, slash))) {
        SetError(win32::kErrorPathNotFound);
        return false;
    }
    Status made = store_.MakeDirectory(Root::kUser, UserPathOf(where.relative));
    if (!made) {
        SetError(win32Error(made.Code(), true));
        return false;
    }
    return true;
}

bool Kernel::RemoveDirectory(std::string_view name) {
    GamePath where = Locate(name);
    if (!where.inGame) {
        SetError(win32::kErrorPathNotFound);
        return false;
    }
    std::optional<Entry> entry = Lookup(where.relative);
    if (!entry) {
        SetError(MissingError(where.relative));
        return false;
    }
    if (!entry->info.isDirectory) {
        SetError(win32::kErrorDirectory);
        return false;
    }
    // The five per-user folders are not looked up in the install, which ships none of them.
    if (where.relative.empty() ||
        (!isUserFolder(where.relative.substr(0, where.relative.find('/'))) &&
         store_.Stat(Root::kInstall, where.relative))) {
        SetError(win32::kErrorAccessDenied);
        return false;
    }
    if (!ListDirectory(where.relative).empty()) {
        SetError(win32::kErrorDirNotEmpty);
        return false;
    }
    std::string user = UserPathOf(where.relative);
    for (const std::string &path : {user, "logs/" + where.relative}) {
        if (store_.Stat(Root::kUser, path)) {
            Status removed = store_.Remove(Root::kUser, path);
            if (!removed) {
                SetError(win32Error(removed.Code(), true));
                return false;
            }
        }
    }
    return true;
}

std::uint32_t Kernel::GetFileAttributes(std::string_view name) {
    GamePath where = Locate(name);
    if (!where.inGame) {
        SetError(win32::kErrorPathNotFound);
        return win32::kInvalidFileAttributes;
    }
    std::optional<Entry> entry = Lookup(where.relative);
    if (!entry) {
        SetError(MissingError(where.relative));
        return win32::kInvalidFileAttributes;
    }
    return attributesOf(entry->info);
}

bool Kernel::SetFileAttributes(std::string_view name, std::uint32_t attributes) {
    (void)attributes; // not kept: Store has no attributes, and the game's files stay writable
    GamePath where = Locate(name);
    if (!where.inGame) {
        SetError(win32::kErrorPathNotFound);
        return false;
    }
    if (!Lookup(where.relative)) {
        SetError(MissingError(where.relative));
        return false;
    }
    return true;
}

} // namespace sfcop::hle::kernel
