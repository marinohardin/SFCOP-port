// The stub backend: compiles on every platform and supports nothing.
#include "platform/Store/Store.h"

namespace sfcop::platform {
namespace {

class StoreStub final : public Store {
public:
    Result<std::string> RootPath(Root) override { return Error::kUnsupported; }
    Result<FileHandle> Open(Root, std::string_view, OpenMode) override {
        return Error::kUnsupported;
    }
    Result<std::size_t> Read(FileHandle, void *, std::size_t) override {
        return Error::kUnsupported;
    }
    Result<std::size_t> Write(FileHandle, const void *, std::size_t) override {
        return Error::kUnsupported;
    }
    Result<std::uint64_t> Seek(FileHandle, std::int64_t, SeekOrigin) override {
        return Error::kUnsupported;
    }
    Result<FileInfo> GetInfo(FileHandle) override { return Error::kUnsupported; }
    Status SetSize(FileHandle, std::uint64_t) override { return Error::kUnsupported; }
    Status SetModifiedTime(FileHandle, std::int64_t) override { return Error::kUnsupported; }
    Status Flush(FileHandle) override { return Error::kUnsupported; }
    Status Close(FileHandle) override { return Error::kUnsupported; }
    Result<FileInfo> Stat(Root, std::string_view) override { return Error::kUnsupported; }
    Result<std::vector<DirectoryEntry>> List(Root, std::string_view) override {
        return Error::kUnsupported;
    }
    Status MakeDirectory(Root, std::string_view) override { return Error::kUnsupported; }
    Status Remove(Root, std::string_view) override { return Error::kUnsupported; }
    Status Rename(Root, std::string_view, std::string_view) override {
        return Error::kUnsupported;
    }
    Status WriteAtomic(Root, std::string_view, const void *, std::size_t) override {
        return Error::kUnsupported;
    }
    Result<bool> KeyExists(std::string_view) override { return Error::kUnsupported; }
    Status CreateKey(std::string_view) override { return Error::kUnsupported; }
    Status DeleteKey(std::string_view) override { return Error::kUnsupported; }
    Result<RegistryValue> GetValue(std::string_view, std::string_view) override {
        return Error::kUnsupported;
    }
    Status SetValue(std::string_view, std::string_view, const RegistryValue &) override {
        return Error::kUnsupported;
    }
};

} // namespace

std::unique_ptr<Store> makeStoreStub() { return std::make_unique<StoreStub>(); }

} // namespace sfcop::platform
