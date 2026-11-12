#include "app/ImportFiles.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace sfcop::app {

namespace {

namespace fs = std::filesystem;

fs::path toPath(const std::string &utf8) { return fs::u8path(utf8); }

class HostFileWriter final : public FileWriter {
public:
    explicit HostFileWriter(const fs::path &path)
        : stream_(path, std::ios::binary | std::ios::trunc) {}

    bool isOpen() const { return stream_.is_open(); }

    bool write(const unsigned char *buffer, std::size_t size) override {
        stream_.write(reinterpret_cast<const char *>(buffer), static_cast<std::streamsize>(size));
        return !stream_.fail();
    }

    bool close() override {
        stream_.close();
        return !stream_.fail();
    }

private:
    std::ofstream stream_;
};

class HostImportFiles final : public ImportFiles {
public:
    std::optional<EntryKind> kindOf(const std::string &path) const override {
        std::error_code error;
        const fs::file_status status = fs::symlink_status(toPath(path), error);
        if (error || !fs::exists(status)) {
            return std::nullopt;
        }
        return kindFrom(status);
    }

    std::optional<EntryKind> kindFollowing(const std::string &path) const override {
        std::error_code error;
        const fs::file_status status = fs::status(toPath(path), error);
        if (error || !fs::exists(status)) {
            return std::nullopt;
        }
        return kindFrom(status);
    }

    std::optional<std::uint64_t> fileSize(const std::string &path) const override {
        std::error_code error;
        const fs::path p = toPath(path);
        if (!fs::is_regular_file(p, error)) {
            return std::nullopt;
        }
        const std::uintmax_t size = fs::file_size(p, error);
        if (error) {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(size);
    }

    std::vector<FolderEntry> list(const std::string &dir) const override {
        std::vector<FolderEntry> entries;
        std::error_code error;
        for (fs::directory_iterator it(toPath(dir), error), end; !error && it != end;
             it.increment(error)) {
            std::error_code statusError;
            const fs::file_status status = it->symlink_status(statusError);
            if (statusError) {
                continue;
            }
            entries.push_back({it->path().filename().u8string(), kindFrom(status)});
        }
        std::sort(entries.begin(), entries.end(),
                  [](const FolderEntry &a, const FolderEntry &b) { return a.name < b.name; });
        return entries;
    }

    std::unique_ptr<FileReader> openRead(const std::string &path) const override {
        return hostFileSystem().openRead(path);
    }

    std::unique_ptr<FileWriter> openWrite(const std::string &path) override {
        auto writer = std::make_unique<HostFileWriter>(toPath(path));
        if (!writer->isOpen()) {
            return nullptr;
        }
        return writer;
    }

    bool clone(const std::string &from, const std::string &to) override {
        return cloneHostFile(from, to);
    }

    bool createDirectories(const std::string &path) override {
        return hostFileSystem().createDirectories(path);
    }

    bool rename(const std::string &from, const std::string &to) override {
        std::error_code error;
        if (fs::exists(fs::symlink_status(toPath(to), error))) {
            return false;
        }
        fs::rename(toPath(from), toPath(to), error);
        return !error;
    }

    bool removeAll(const std::string &path) override {
        std::error_code error;
        fs::remove_all(toPath(path), error);
        return !fs::exists(fs::symlink_status(toPath(path), error));
    }

    std::optional<std::uint64_t> freeBytes(const std::string &path) const override {
        std::error_code error;
        fs::path p = toPath(path);
        while (!p.empty() && !fs::exists(p, error)) {
            const fs::path parent = p.parent_path();
            if (parent == p) {
                break;
            }
            p = parent;
        }
        if (p.empty()) {
            p = fs::current_path(error);
        }
        const fs::space_info space = fs::space(p, error);
        if (error) {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(space.available);
    }

private:
    static EntryKind kindFrom(const fs::file_status &status) {
        if (fs::is_regular_file(status)) {
            return EntryKind::kFile;
        }
        if (fs::is_directory(status)) {
            return EntryKind::kFolder;
        }
        return EntryKind::kOther;
    }
};

} // namespace

ImportFiles &hostImportFiles() {
    static HostImportFiles files;
    return files;
}

} // namespace sfcop::app
