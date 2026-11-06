// LogsStore: a Store whose per-user `logs/` folder lives somewhere else (the client's --logs DIR,
// DESIGN §6.8). The kernel module sends every `.txt` and `.log` file the game writes beside the
// exe to `logs/` of the per-user folder (Kernel.h, "the game's path model"); a run that must read
// such a file afterwards (the replay oracle's OracleTrace.txt) is given a folder of its own for
// them, so that nothing else in the per-user folder is touched or read.
//
// Routing, by path under Root::kUser: `logs` and `logs/<rest>` (any case) go to the logs Store
// as `` (its root) and `<rest>`; everything else, Root::kInstall and the registry go to the
// per-user Store. A handle this Store hands out is its own; it remembers which inner Store
// opened it. A rename between the two sides is refused (kAccessDenied): the game never moves a
// file across them.
//
// Threading: as Store's, with one lock over the handle table.
#pragma once

#include "platform/Store/Store.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace sfcop::hle::kernel {

class LogsStore final : public platform::Store {
public:
    LogsStore(std::unique_ptr<platform::Store> user, std::unique_ptr<platform::Store> logs);

    platform::Result<std::string> RootPath(platform::Root root) override;
    platform::Result<platform::FileHandle> Open(platform::Root root, std::string_view path,
                                                platform::OpenMode mode) override;
    platform::Result<std::size_t> Read(platform::FileHandle file, void *out,
                                       std::size_t bytes) override;
    platform::Result<std::size_t> Write(platform::FileHandle file, const void *data,
                                        std::size_t bytes) override;
    platform::Result<std::uint64_t> Seek(platform::FileHandle file, std::int64_t offset,
                                         platform::SeekOrigin origin) override;
    platform::Result<platform::FileInfo> GetInfo(platform::FileHandle file) override;
    platform::Status SetSize(platform::FileHandle file, std::uint64_t bytes) override;
    platform::Status SetModifiedTime(platform::FileHandle file, std::int64_t modifiedNs) override;
    platform::Status Flush(platform::FileHandle file) override;
    platform::Status Close(platform::FileHandle file) override;
    platform::Result<platform::FileInfo> Stat(platform::Root root, std::string_view path) override;
    platform::Result<std::vector<platform::DirectoryEntry>> List(platform::Root root,
                                                                 std::string_view path) override;
    platform::Status MakeDirectory(platform::Root root, std::string_view path) override;
    platform::Status Remove(platform::Root root, std::string_view path) override;
    platform::Status Rename(platform::Root root, std::string_view from,
                            std::string_view to) override;
    platform::Status WriteAtomic(platform::Root root, std::string_view path, const void *data,
                                 std::size_t bytes) override;
    platform::Result<bool> KeyExists(std::string_view key) override;
    platform::Status CreateKey(std::string_view key) override;
    platform::Status DeleteKey(std::string_view key) override;
    platform::Result<platform::RegistryValue> GetValue(std::string_view key,
                                                       std::string_view name) override;
    platform::Status SetValue(std::string_view key, std::string_view name,
                              const platform::RegistryValue &value) override;

    // Where a per-user path goes: true when it is `logs` or under it, with `rest` the path
    // inside the logs folder (empty for the folder itself).
    static bool IsLogsPath(std::string_view path, std::string_view *rest);

private:
    struct Routed {
        platform::Store *store;
        std::string path;
    };
    struct Inner {
        platform::Store *store;
        platform::FileHandle handle;
    };

    Routed Route(platform::Root root, std::string_view path) const;
    // The inner Store and handle behind one of this Store's handles, or null.
    Inner *Find(platform::FileHandle file);

    std::unique_ptr<platform::Store> user_;
    std::unique_ptr<platform::Store> logs_;
    std::mutex lock_;
    std::map<std::uint32_t, Inner> open_;
    std::uint32_t next_ = 1;
};

} // namespace sfcop::hle::kernel
