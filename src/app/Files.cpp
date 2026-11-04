#include "app/Files.h"

#include <filesystem>
#include <fstream>
#include <system_error>

namespace sfcop::app {

namespace {

namespace fs = std::filesystem;

fs::path toPath(const std::string &utf8) { return fs::u8path(utf8); }

class HostFileReader final : public FileReader {
public:
    explicit HostFileReader(const fs::path &path) : stream_(path, std::ios::binary) {}

    bool isOpen() const { return stream_.is_open(); }

    std::size_t read(unsigned char *buffer, std::size_t size) override {
        if (stream_.bad() || stream_.eof()) {
            return 0;
        }
        stream_.read(reinterpret_cast<char *>(buffer), static_cast<std::streamsize>(size));
        return stream_.bad() ? 0 : static_cast<std::size_t>(stream_.gcount());
    }

    bool failed() const override { return stream_.bad(); }

private:
    std::ifstream stream_;
};

class HostFileSystem final : public FileSystem {
public:
    bool isDirectory(const std::string &path) const override {
        std::error_code error;
        return fs::is_directory(toPath(path), error);
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

    std::unique_ptr<FileReader> openRead(const std::string &path) const override {
        auto reader = std::make_unique<HostFileReader>(toPath(path));
        if (!reader->isOpen()) {
            return nullptr;
        }
        return reader;
    }

    bool createDirectories(const std::string &path) override {
        std::error_code error;
        fs::create_directories(toPath(path), error);
        return fs::is_directory(toPath(path), error);
    }

    std::optional<std::string> readText(const std::string &path) const override {
        std::unique_ptr<FileReader> reader = openRead(path);
        if (!reader) {
            return std::nullopt;
        }
        std::string text;
        unsigned char buffer[4096];
        while (const std::size_t got = reader->read(buffer, sizeof buffer)) {
            text.append(reinterpret_cast<const char *>(buffer), got);
        }
        if (reader->failed()) {
            return std::nullopt;
        }
        return text;
    }

    bool writeTextAtomic(const std::string &path, const std::string &text) override {
        const std::string temporary = path + ".tmp";
        bool written = false;
        {
            std::ofstream out(toPath(temporary), std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                return false;
            }
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.close();
            written = !out.fail();
        }
        std::error_code error;
        if (!written) {
            fs::remove(toPath(temporary), error);
            return false;
        }
        fs::rename(toPath(temporary), toPath(path), error);
        if (error) {
            fs::remove(toPath(temporary), error);
            return false;
        }
        return true;
    }

    std::string absolutePath(const std::string &path) const override {
        const fs::path p = toPath(path);
        if (p.is_absolute()) {
            return path;
        }
        std::error_code error;
        const fs::path full = fs::absolute(p, error);
        if (error) {
            return path;
        }
        std::string normal = full.lexically_normal().u8string();
        if (normal.size() > 1 && (normal.back() == '/' || normal.back() == '\\')) {
            normal.pop_back();
        }
        return normal;
    }
};

} // namespace

FileSystem &hostFileSystem() {
    static HostFileSystem instance;
    return instance;
}

std::string joinPath(const std::string &dir, const std::string &relative) {
    if (dir.empty()) {
        return relative;
    }
    const bool backslashes =
        dir.find('\\') != std::string::npos && dir.find('/') == std::string::npos;
    std::string tail = relative;
    if (backslashes) {
        for (char &c : tail) {
            c = c == '/' ? '\\' : c;
        }
    }
    const char last = dir.back();
    if (last == '/' || last == '\\') {
        return dir + tail;
    }
    return dir + (backslashes ? "\\" : "/") + tail;
}

} // namespace sfcop::app
