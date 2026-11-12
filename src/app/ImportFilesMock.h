// A test double for ImportFiles (ImportFiles.h): the real file system, with the free space, the
// clone, writes and renames under the test's control, and every change counted. Tests only.
#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include "app/ImportFiles.h"

namespace sfcop::app::testing {

class ControlledImportFiles final : public ImportFiles {
public:
    // Reported as the free space when set; the volume's own otherwise.
    std::optional<std::uint64_t> free;
    // Whether clone() clones: false makes the import copy; true clones by copying through the
    // host, so the test does not depend on the volume it runs on.
    bool canClone = false;
    // The write that fails, counted from 1 over every FileWriter::write; 0 for none.
    std::size_t failWrite = 0;
    // The rename that fails, counted from 1 over every rename; 0 for none.
    std::size_t failRename = 0;
    std::size_t renames = 0;
    // FileWriter::write calls so far.
    std::size_t writes = 0;
    // Every call that changes the file system.
    std::size_t changes = 0;

    std::optional<EntryKind> kindOf(const std::string &path) const override {
        return host().kindOf(path);
    }
    std::optional<EntryKind> kindFollowing(const std::string &path) const override {
        return host().kindFollowing(path);
    }
    std::optional<std::uint64_t> fileSize(const std::string &path) const override {
        return host().fileSize(path);
    }
    std::vector<FolderEntry> list(const std::string &dir) const override {
        return host().list(dir);
    }
    std::unique_ptr<FileReader> openRead(const std::string &path) const override {
        return host().openRead(path);
    }
    std::unique_ptr<FileWriter> openWrite(const std::string &path) override {
        ++changes;
        std::unique_ptr<FileWriter> inner = host().openWrite(path);
        if (!inner) {
            return nullptr;
        }
        return std::make_unique<CountingWriter>(std::move(inner), *this);
    }
    bool clone(const std::string &from, const std::string &to) override {
        if (!canClone) {
            return false;
        }
        ++changes;
        ++clones;
        std::unique_ptr<FileReader> reader = host().openRead(from);
        std::unique_ptr<FileWriter> writer = host().openWrite(to);
        unsigned char buffer[4096];
        while (const std::size_t got = reader->read(buffer, sizeof buffer)) {
            writer->write(buffer, got);
        }
        return writer->close();
    }
    bool createDirectories(const std::string &path) override {
        ++changes;
        return host().createDirectories(path);
    }
    bool rename(const std::string &from, const std::string &to) override {
        ++changes;
        if (++renames == failRename) {
            return false;
        }
        return host().rename(from, to);
    }
    bool removeAll(const std::string &path) override {
        if (host().kindOf(path)) {
            ++changes;
        }
        return host().removeAll(path);
    }
    std::optional<std::uint64_t> freeBytes(const std::string &path) const override {
        return free ? free : host().freeBytes(path);
    }

    std::size_t clones = 0;

private:
    class CountingWriter final : public FileWriter {
    public:
        CountingWriter(std::unique_ptr<FileWriter> inner, ControlledImportFiles &owner)
            : inner_(std::move(inner)), owner_(owner) {}
        bool write(const unsigned char *buffer, std::size_t size) override {
            if (++owner_.writes == owner_.failWrite) {
                return false;
            }
            return inner_->write(buffer, size);
        }
        bool close() override { return inner_->close(); }

    private:
        std::unique_ptr<FileWriter> inner_;
        ControlledImportFiles &owner_;
    };

    static ImportFiles &host() { return hostImportFiles(); }
};

} // namespace sfcop::app::testing
