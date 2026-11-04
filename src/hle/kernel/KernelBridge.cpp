#include "hle/kernel/KernelBridge.h"
#include "hle/kernel/KernelBridgeTesting.h"

#include "app/DataDir.h"
#include "app/HostPlatform.h"
#include "hle/kernel/Kernel.h"
#include "hle/kernel/Text.h"
#include "platform/Store/Store.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>

namespace {

using sfcop::hle::kernel::FindData;
using sfcop::hle::kernel::Kernel;
namespace text = sfcop::hle::kernel;
namespace win32 = sfcop::hle::kernel::win32;

constexpr std::uint32_t kErrorBadLength = 24;
constexpr std::uint32_t kErrorInsufficientBuffer = 122;

struct Attached {
    std::unique_ptr<sfcop::platform::Store> store;
    std::unique_ptr<Kernel> kernel;
};

std::mutex &lock() {
    static std::mutex mutex;
    return mutex;
}

Attached &attached() {
    static Attached state;
    return state;
}

// The kernel, or none when nothing is attached (every call then fails as a missing path would).
Kernel *kernel() { return attached().kernel.get(); }

// The Win32 last error of the calling host thread. The lifted game runs its own threads as host
// threads, so the error one thread's call set must reach that thread's GetLastError and no
// other's: a call leaves its error here before it releases the lock, and the shim reads it back
// on the same thread (sfcop_kernel_take_error).
std::optional<std::uint32_t> &threadError() {
    thread_local std::optional<std::uint32_t> error;
    return error;
}

// Moves the kernel's last error to the calling thread's when the call that set it ends, still
// under the lock, so another thread's call cannot take or overwrite it in between.
struct HandOver {
    Kernel *kernel;
    ~HandOver() {
        std::optional<std::uint32_t> error = kernel->TakeLastError();
        if (error) {
            threadError() = error;
        }
    }
};

// An exception must not unwind into the shim (C, in the lifted game's frames). Each entry logs its
// first failure by name (never the exception's text, which can carry a path) and answers the
// Win32 failure: ERROR_NOT_ENOUGH_MEMORY for an allocation, ERROR_GEN_FAILURE otherwise.
void noteException(const char *entry, std::uint32_t code) {
    static std::set<std::string> logged;
    threadError() = code;
    if (logged.insert(entry).second) {
        std::fprintf(stderr, "[hle-kernel] %s: an exception was caught; answering Win32 error %u\n",
                     entry, static_cast<unsigned>(code));
    }
}

// Runs `body` on the attached kernel, under the lock. `failure` is the answer when nothing is
// attached or the body throws.
template <typename R, typename Body> R guarded(const char *entry, R failure, Body body) {
    std::lock_guard<std::mutex> guard(lock());
    Kernel *k = kernel();
    if (k == nullptr) {
        threadError() = win32::kErrorPathNotFound;
        return failure;
    }
    try {
        HandOver handOver{k};
        return body(k);
    } catch (const std::bad_alloc &) {
        noteException(entry, win32::kErrorNotEnoughMemory);
    } catch (const std::exception &) {
        noteException(entry, win32::kErrorGenFailure);
    } catch (...) {
        noteException(entry, win32::kErrorGenFailure);
    }
    return failure;
}

// A game string as UTF-8: ANSI bytes (Latin-1) or UTF-16. NULL is the empty string.
std::string utf8Of(const void *name, int wide) {
    if (name == nullptr) {
        return std::string();
    }
    if (wide != 0) {
        return text::utf16ToUtf8(text::wideFrom(static_cast<const std::uint16_t *>(name)));
    }
    return text::ansiToUtf8(static_cast<const char *>(name));
}

// The answer of the calls that fill a buffer of `size` characters: the length without the NUL
// when it fits, else the size needed with it.
std::uint32_t answer(const std::string &utf8, void *buffer, std::uint32_t size, int wide) {
    if (wide != 0) {
        return text::answerWide(text::utf8ToUtf16(utf8), static_cast<std::uint16_t *>(buffer), size);
    }
    return text::answerString(text::utf8ToAnsi(utf8), static_cast<char *>(buffer), size);
}

void put32(std::uint8_t *at, std::uint32_t value) {
    at[0] = static_cast<std::uint8_t>(value);
    at[1] = static_cast<std::uint8_t>(value >> 8);
    at[2] = static_cast<std::uint8_t>(value >> 16);
    at[3] = static_cast<std::uint8_t>(value >> 24);
}

void put64(std::uint8_t *at, std::uint64_t value) {
    put32(at, static_cast<std::uint32_t>(value));
    put32(at + 4, static_cast<std::uint32_t>(value >> 32));
}

// WIN32_FIND_DATAA/W: attributes at 0, the three FILETIMEs at 4, 12 and 20, the size's high and
// low halves at 28 and 32, two reserved words, then cFileName[260] at 44 and
// cAlternateFileName[14] after it (at 304 in the A form, 564 in the W form; left empty).
void writeFindData(const FindData &entry, std::uint8_t *data, int wide) {
    std::memset(data, 0, wide != 0 ? SFCOP_FIND_DATA_W_SIZE : SFCOP_FIND_DATA_A_SIZE);
    put32(data, entry.attributes);
    put64(data + 4, entry.creationTime);
    put64(data + 12, entry.accessTime);
    put64(data + 20, entry.writeTime);
    put32(data + 28, static_cast<std::uint32_t>(entry.size >> 32));
    put32(data + 32, static_cast<std::uint32_t>(entry.size));
    if (wide != 0) {
        std::u16string name = text::utf8ToUtf16(entry.name);
        std::size_t count = name.size() < 259 ? name.size() : 259;
        for (std::size_t i = 0; i < count; i++) {
            data[44 + 2 * i] = static_cast<std::uint8_t>(name[i]);
            data[45 + 2 * i] = static_cast<std::uint8_t>(name[i] >> 8);
        }
    } else {
        std::string name = text::utf8ToAnsi(entry.name);
        std::memcpy(data + 44, name.data(), name.size() < 259 ? name.size() : 259);
    }
}

// The fixed-size name buffers of GetVolumeInformation: NULL is not asked for.
bool fillName(const std::string &utf8, void *buffer, std::uint32_t size, int wide) {
    if (buffer == nullptr) {
        return true;
    }
    std::uint32_t answered = answer(utf8, buffer, size, wide);
    return answered < size;
}

} // namespace

extern "C" {

void sfcop_hle_kernel_attach(const char *installDir, const char *userDir) {
    std::lock_guard<std::mutex> guard(lock());
    Attached &state = attached();
    state.kernel.reset();
    state.store.reset();
    try {
        std::string user = userDir != nullptr ? userDir : "";
        if (!user.empty()) {
            std::error_code ignored;
            std::filesystem::create_directories(user, ignored);
        }
        state.store = sfcop::platform::makeStore(installDir != nullptr ? installDir : "", user);
        state.kernel = std::make_unique<Kernel>(*state.store);
    } catch (...) {
        // Left detached: every call then fails as a missing path would.
        state.kernel.reset();
        state.store.reset();
        noteException("sfcop_hle_kernel_attach", win32::kErrorGenFailure);
    }
}

void sfcop_hle_kernel_detach(void) {
    std::lock_guard<std::mutex> guard(lock());
    attached().kernel.reset();
    attached().store.reset();
}

int sfcop_hle_kernel_attached(void) {
    std::lock_guard<std::mutex> guard(lock());
    return kernel() != nullptr ? 1 : 0;
}

size_t sfcop_hle_default_user_dir(char *out, size_t size) {
    // The client decides the per-user folder (src/app/Startup.h); asking DataDir.h for its default
    // keeps a module attached on first use in the same place.
    try {
        sfcop::app::DataDirResult found = sfcop::app::resolveDataDir(
            sfcop::app::hostPlatform(), sfcop::app::hostEnvironment(), std::nullopt);
        if (!found.dir || found.dir->size() + 1 > size) {
            return 0;
        }
        std::memcpy(out, found.dir->c_str(), found.dir->size() + 1);
        return found.dir->size();
    } catch (...) {
        return 0;
    }
}

int sfcop_kernel_take_error(uint32_t *code) {
    // The calling thread's own, set by its last call; no lock, no other thread's error.
    std::optional<std::uint32_t> error = threadError();
    threadError().reset();
    if (!error) {
        return 0;
    }
    *code = *error;
    return 1;
}

uint32_t sfcop_kernel_current_directory(void *buffer, uint32_t size, int wide) {
    return guarded("sfcop_kernel_current_directory", uint32_t(0), [&](Kernel *k) -> uint32_t {
        return answer(k->CurrentDirectory(), buffer, size, wide);
    });
}

uint32_t sfcop_kernel_windows_directory(void *buffer, uint32_t size, int wide, int system) {
    return guarded("sfcop_kernel_windows_directory", uint32_t(0), [&](Kernel *k) -> uint32_t {
        return answer(system != 0 ? k->SystemDirectory() : k->WindowsDirectory(), buffer, size, wide);
    });
}

int sfcop_kernel_set_current_directory(const void *name, int wide) {
    return guarded("sfcop_kernel_set_current_directory", int(0), [&](Kernel *k) -> int {
        return k->SetCurrentDirectory(utf8Of(name, wide)) ? 1 : 0;
    });
}

uint32_t sfcop_kernel_module_file_name(char *buffer, uint32_t size) {
    return guarded("sfcop_kernel_module_file_name", uint32_t(0), [&](Kernel *k) -> uint32_t {
        std::string path = text::utf8ToAnsi(k->ModuleFileName());
        std::uint32_t copied = text::copyTruncated(path, buffer, size);
        if (path.size() + 1 > size) {
            // Cut short: the buffer's size comes back, with ERROR_INSUFFICIENT_BUFFER.
            threadError() = kErrorInsufficientBuffer;
            return size;
        }
        return copied;
    });
}

uint32_t sfcop_kernel_full_path_name(const char *name, char *buffer, uint32_t size,
                                     uint32_t *filePart) {
    return guarded("sfcop_kernel_full_path_name", uint32_t(0), [&](Kernel *k) -> uint32_t {
        *filePart = UINT32_MAX;
        std::size_t part = std::string::npos;
        std::optional<std::string> full = k->FullPathName(utf8Of(name, 0), &part);
        if (!full) {
            return 0;
        }
        std::string ansi = text::utf8ToAnsi(*full);
        std::uint32_t answered = text::answerString(ansi, buffer, size);
        if (answered < size && part != std::string::npos) {
            // The offset in the ANSI form: count the UTF-8 prefix's characters.
            *filePart = static_cast<std::uint32_t>(text::utf8ToAnsi(full->substr(0, part)).size());
        }
        return answered;
    });
}

uint32_t sfcop_kernel_short_path_name(const char *name, char *buffer, uint32_t size) {
    return guarded("sfcop_kernel_short_path_name", uint32_t(0), [&](Kernel *k) -> uint32_t {
        std::optional<std::string> path = k->ShortPathName(utf8Of(name, 0));
        if (!path) {
            return 0;
        }
        return text::answerString(text::utf8ToAnsi(*path), buffer, size);
    });
}

uint32_t sfcop_kernel_drive_type(const void *root, int wide) {
    return guarded("sfcop_kernel_drive_type", uint32_t(win32::kDriveNoRootDir), [&](Kernel *k) -> uint32_t {
        return k->DriveType(utf8Of(root, wide));
    });
}

int sfcop_kernel_disk_free_space(const void *root, int wide, uint32_t *sectorsPerCluster,
                                 uint32_t *bytesPerSector, uint32_t *freeClusters,
                                 uint32_t *totalClusters) {
    return guarded("sfcop_kernel_disk_free_space", int(0), [&](Kernel *k) -> int {
        return k->DiskFreeSpace(utf8Of(root, wide), sectorsPerCluster, bytesPerSector, freeClusters,
                                totalClusters)
                   ? 1
                   : 0;
    });
}

int sfcop_kernel_volume_information(const void *root, int wide, void *volumeName,
                                    uint32_t volumeSize, uint32_t *serial,
                                    uint32_t *maxComponentLength, uint32_t *flags,
                                    void *fileSystem, uint32_t fileSystemSize) {
    return guarded("sfcop_kernel_volume_information", int(0), [&](Kernel *k) -> int {
        std::optional<Kernel::VolumeInfo> info = k->VolumeInformation(utf8Of(root, wide));
        if (!info) {
            return 0;
        }
        if (!fillName(info->name, volumeName, volumeSize, wide) ||
            !fillName(info->fileSystem, fileSystem, fileSystemSize, wide)) {
            threadError() = kErrorBadLength;
            k->TakeLastError();
            return 0;
        }
        if (serial != nullptr) {
            *serial = info->serial;
        }
        if (maxComponentLength != nullptr) {
            *maxComponentLength = info->maxComponentLength;
        }
        if (flags != nullptr) {
            *flags = info->flags;
        }
        return 1;
    });
}

uint32_t sfcop_kernel_create_file(const void *name, int wide, uint32_t access, uint32_t share,
                                  uint32_t disposition, uint32_t flags) {
    return guarded("sfcop_kernel_create_file", uint32_t(0), [&](Kernel *k) -> uint32_t {
        return k->CreateFile(utf8Of(name, wide), access, share, disposition, flags);
    });
}

int sfcop_kernel_is_file(uint32_t file) {
    std::lock_guard<std::mutex> guard(lock());
    return kernel() != nullptr && kernel()->IsFile(file) ? 1 : 0;
}

int sfcop_kernel_read_file(uint32_t file, void *out, uint32_t bytes, uint32_t *read) {
    return guarded("sfcop_kernel_read_file", int(0), [&](Kernel *k) -> int {
        return k->ReadFile(file, out, bytes, read) ? 1 : 0;
    });
}

int sfcop_kernel_write_file(uint32_t file, const void *data, uint32_t bytes, uint32_t *written) {
    return guarded("sfcop_kernel_write_file", int(0), [&](Kernel *k) -> int {
        return k->WriteFile(file, data, bytes, written) ? 1 : 0;
    });
}

uint32_t sfcop_kernel_set_file_pointer(uint32_t file, int32_t distance, int32_t *high,
                                       uint32_t method) {
    return guarded("sfcop_kernel_set_file_pointer", uint32_t(win32::kInvalidSetFilePointer), [&](Kernel *k) -> uint32_t {
        return k->SetFilePointer(file, distance, high, method);
    });
}

uint32_t sfcop_kernel_get_file_size(uint32_t file, uint32_t *high) {
    return guarded("sfcop_kernel_get_file_size", uint32_t(win32::kInvalidFileSize), [&](Kernel *k) -> uint32_t {
        return k->GetFileSize(file, high);
    });
}

int sfcop_kernel_get_file_time(uint32_t file, uint64_t *creation, uint64_t *access,
                               uint64_t *write) {
    return guarded("sfcop_kernel_get_file_time", int(0), [&](Kernel *k) -> int {
        return k->GetFileTime(file, creation, access, write) ? 1 : 0;
    });
}

int sfcop_kernel_set_file_time(uint32_t file, const uint64_t *creation, const uint64_t *access,
                               const uint64_t *write) {
    return guarded("sfcop_kernel_set_file_time", int(0), [&](Kernel *k) -> int {
        return k->SetFileTime(file, creation, access, write) ? 1 : 0;
    });
}

int sfcop_kernel_flush_file(uint32_t file) {
    return guarded("sfcop_kernel_flush_file", int(0), [&](Kernel *k) -> int {
        return k->FlushFileBuffers(file) ? 1 : 0;
    });
}

int sfcop_kernel_set_end_of_file(uint32_t file) {
    return guarded("sfcop_kernel_set_end_of_file", int(0), [&](Kernel *k) -> int {
        return k->SetEndOfFile(file) ? 1 : 0;
    });
}

int sfcop_kernel_close_file(uint32_t file) {
    return guarded("sfcop_kernel_close_file", int(0), [&](Kernel *k) -> int {
        return k->CloseFile(file) ? 1 : 0;
    });
}

int sfcop_kernel_delete_file(const void *name, int wide) {
    return guarded("sfcop_kernel_delete_file", int(0), [&](Kernel *k) -> int {
        return k->DeleteFile(utf8Of(name, wide)) ? 1 : 0;
    });
}

int sfcop_kernel_move_file(const char *from, const char *to) {
    return guarded("sfcop_kernel_move_file", int(0), [&](Kernel *k) -> int {
        return k->MoveFile(utf8Of(from, 0), utf8Of(to, 0)) ? 1 : 0;
    });
}

int sfcop_kernel_copy_file(const char *from, const char *to, int failIfExists) {
    return guarded("sfcop_kernel_copy_file", int(0), [&](Kernel *k) -> int {
        return k->CopyFile(utf8Of(from, 0), utf8Of(to, 0), failIfExists != 0) ? 1 : 0;
    });
}

int sfcop_kernel_create_directory(const void *name, int wide) {
    return guarded("sfcop_kernel_create_directory", int(0), [&](Kernel *k) -> int {
        return k->CreateDirectory(utf8Of(name, wide)) ? 1 : 0;
    });
}

int sfcop_kernel_remove_directory(const void *name, int wide) {
    return guarded("sfcop_kernel_remove_directory", int(0), [&](Kernel *k) -> int {
        return k->RemoveDirectory(utf8Of(name, wide)) ? 1 : 0;
    });
}

uint32_t sfcop_kernel_get_file_attributes(const void *name, int wide) {
    return guarded("sfcop_kernel_get_file_attributes", uint32_t(win32::kInvalidFileAttributes), [&](Kernel *k) -> uint32_t {
        return k->GetFileAttributes(utf8Of(name, wide));
    });
}

int sfcop_kernel_set_file_attributes(const void *name, int wide, uint32_t attributes) {
    return guarded("sfcop_kernel_set_file_attributes", int(0), [&](Kernel *k) -> int {
        return k->SetFileAttributes(utf8Of(name, wide), attributes) ? 1 : 0;
    });
}

uint32_t sfcop_kernel_find_first(const void *pattern, int wide, uint8_t *data) {
    return guarded("sfcop_kernel_find_first", uint32_t(0), [&](Kernel *k) -> uint32_t {
        FindData first;
        std::uint32_t find = k->FindFirstFile(utf8Of(pattern, wide), &first);
        if (find != 0 && data != nullptr) {
            writeFindData(first, data, wide);
        }
        return find;
    });
}

int sfcop_kernel_find_next(uint32_t find, uint8_t *data, int wide) {
    return guarded("sfcop_kernel_find_next", int(0), [&](Kernel *k) -> int {
        FindData next;
        if (!k->FindNextFile(find, &next)) {
            return 0;
        }
        if (data != nullptr) {
            writeFindData(next, data, wide);
        }
        return 1;
    });
}

int sfcop_kernel_find_close(uint32_t find) {
    return guarded("sfcop_kernel_find_close", int(0), [&](Kernel *k) -> int {
        return k->FindClose(find) ? 1 : 0;
    });
}

int sfcop_kernel_is_find(uint32_t find) {
    std::lock_guard<std::mutex> guard(lock());
    return kernel() != nullptr && kernel()->IsFind(find) ? 1 : 0;
}

uint32_t sfcop_kernel_get_profile_string(const char *section, const char *key,
                                         const char *fallback, char *buffer, uint32_t size,
                                         const char *file) {
    return guarded("sfcop_kernel_get_profile_string", uint32_t(0), [&](Kernel *k) -> uint32_t {
        return k->GetPrivateProfileString(section, key, fallback, buffer, size, utf8Of(file, 0));
    });
}

uint32_t sfcop_kernel_get_profile_int(const char *section, const char *key, int32_t fallback,
                                      const char *file) {
    return guarded("sfcop_kernel_get_profile_int", uint32_t(static_cast<uint32_t>(fallback)), [&](Kernel *k) -> uint32_t {
        return k->GetPrivateProfileInt(section, key, fallback, utf8Of(file, 0));
    });
}

int sfcop_kernel_write_profile_string(const char *section, const char *key, const char *value,
                                      const char *file) {
    return guarded("sfcop_kernel_write_profile_string", int(0), [&](Kernel *k) -> int {
        return k->WritePrivateProfileString(section, key, value, utf8Of(file, 0)) ? 1 : 0;
    });
}

} // extern "C"

namespace sfcop::hle::kernel::test {

void attachKernelStore(std::unique_ptr<platform::Store> store) {
    std::lock_guard<std::mutex> guard(lock());
    Attached &state = attached();
    state.kernel.reset();
    state.store = std::move(store);
    state.kernel = std::make_unique<Kernel>(*state.store);
}

} // namespace sfcop::hle::kernel::test
