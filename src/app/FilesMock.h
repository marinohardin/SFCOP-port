// Test doubles for the application skeleton's file interface (Files.h), and where the fixtures
// are. Tests only.
#pragma once

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "app/Files.h"

namespace sfcop::app::testing {

// tests/fixtures/ of this checkout, found from this header's own path.
inline std::string fixturesDir() {
    std::string here = __FILE__;
    for (int up = 0; up < 3; ++up) {
        here = here.substr(0, here.find_last_of("/\\"));
    }
    return here + "/tests/fixtures";
}

class MemoryReader final : public FileReader {
public:
    MemoryReader(std::string bytes, std::size_t failAfter)
        : bytes_(std::move(bytes)), failAfter_(failAfter) {}

    std::size_t read(unsigned char *buffer, std::size_t size) override {
        if (failed_) {
            return 0;
        }
        if (offset_ >= failAfter_) {
            failed_ = true;
            return 0;
        }
        const std::size_t take = std::min({size, bytes_.size() - offset_, failAfter_ - offset_});
        std::memcpy(buffer, bytes_.data() + offset_, take);
        offset_ += take;
        return take;
    }

    bool failed() const override { return failed_; }

private:
    std::string bytes_;
    std::size_t failAfter_;
    std::size_t offset_ = 0;
    bool failed_ = false;
};

// Files and folders held in memory, keyed by the exact path string.
class MemoryFileSystem final : public FileSystem {
public:
    std::map<std::string, std::string> files;
    std::set<std::string> dirs;
    // Files that exist but cannot be opened.
    std::set<std::string> unopenable;
    // Files whose reads fail once this many bytes have been read.
    std::map<std::string, std::size_t> failAfter;
    // Every createDirectories and writeTextAtomic call, in order.
    std::vector<std::string> created;
    std::vector<std::string> written;
    bool refuseWrites = false;

    bool isDirectory(const std::string &path) const override { return dirs.count(path) != 0; }

    std::optional<std::uint64_t> fileSize(const std::string &path) const override {
        const auto found = files.find(path);
        if (found == files.end()) {
            return std::nullopt;
        }
        return found->second.size();
    }

    std::unique_ptr<FileReader> openRead(const std::string &path) const override {
        const auto found = files.find(path);
        if (found == files.end() || unopenable.count(path) != 0) {
            return nullptr;
        }
        const auto limit = failAfter.find(path);
        return std::make_unique<MemoryReader>(
            found->second, limit == failAfter.end() ? std::string::npos : limit->second);
    }

    bool createDirectories(const std::string &path) override {
        created.push_back(path);
        if (refuseWrites) {
            return false;
        }
        dirs.insert(path);
        return true;
    }

    std::optional<std::string> readText(const std::string &path) const override {
        const auto found = files.find(path);
        if (found == files.end()) {
            return std::nullopt;
        }
        return found->second;
    }

    bool writeTextAtomic(const std::string &path, const std::string &text) override {
        written.push_back(path);
        if (refuseWrites) {
            return false;
        }
        files[path] = text;
        return true;
    }

    // A relative path is taken against `currentDir`.
    std::string currentDir = "/cwd";
    std::string absolutePath(const std::string &path) const override {
        return !path.empty() && path[0] == '/' ? path : currentDir + "/" + path;
    }
};

// The real file system, with one byte of one file read back changed.
class OneByteAltered final : public FileSystem {
public:
    OneByteAltered(const FileSystem &inner, std::string path, std::size_t offset)
        : inner_(inner), path_(std::move(path)), offset_(offset) {}

    bool isDirectory(const std::string &path) const override { return inner_.isDirectory(path); }

    std::optional<std::uint64_t> fileSize(const std::string &path) const override {
        return inner_.fileSize(path);
    }

    std::unique_ptr<FileReader> openRead(const std::string &path) const override {
        if (path != path_) {
            return inner_.openRead(path);
        }
        std::optional<std::string> bytes = inner_.readText(path);
        if (!bytes || bytes->size() <= offset_) {
            return nullptr;
        }
        (*bytes)[offset_] = static_cast<char>((*bytes)[offset_] ^ 0x01);
        return std::make_unique<MemoryReader>(*bytes, std::string::npos);
    }

    bool createDirectories(const std::string &) override { return false; }

    std::optional<std::string> readText(const std::string &path) const override {
        return inner_.readText(path);
    }

    bool writeTextAtomic(const std::string &, const std::string &) override { return false; }

    std::string absolutePath(const std::string &path) const override {
        return inner_.absolutePath(path);
    }

private:
    const FileSystem &inner_;
    std::string path_;
    std::size_t offset_;
};

} // namespace sfcop::app::testing
