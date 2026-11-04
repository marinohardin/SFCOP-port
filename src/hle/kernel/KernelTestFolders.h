// For the kernel module's tests: an install and a per-user folder under a fresh temporary
// directory (removed afterwards), the host Store on them, and a Kernel on that Store.
#pragma once

#include "hle/kernel/Kernel.h"
#include "platform/Store/Store.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>

namespace sfcop::hle::kernel::test {

struct TestFolders {
    std::filesystem::path base;
    std::filesystem::path install;
    std::filesystem::path user;

    TestFolders() {
        base = std::filesystem::temp_directory_path() /
               ("sfcop-kernel-test-" + std::to_string(std::random_device{}()));
        install = base / "install";
        user = base / "user";
        std::filesystem::create_directories(install);
        std::filesystem::create_directories(user);
    }
    ~TestFolders() {
        std::error_code ignored;
        std::filesystem::remove_all(base, ignored);
    }
    TestFolders(const TestFolders &) = delete;
    TestFolders &operator=(const TestFolders &) = delete;
};

struct GameFolders : TestFolders {
    std::unique_ptr<platform::Store> store;
    Kernel kernel;

    explicit GameFolders(KernelConfig config = {})
        : store(platform::makeStore(install.string(), user.string())),
          kernel(*store, std::move(config)) {}

    static void Write(const std::filesystem::path &path, const std::string &content) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out << content;
    }
    static std::string Read(const std::filesystem::path &path) {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream content;
        content << in.rdbuf();
        return content.str();
    }
    void InstallFile(const std::string &relative, const std::string &content) const {
        Write(install / relative, content);
    }
    std::string InstallContent(const std::string &relative) const { return Read(install / relative); }
    std::string UserContent(const std::string &relative) const { return Read(user / relative); }
    bool UserHas(const std::string &relative) const {
        return std::filesystem::exists(user / relative);
    }
    bool InstallHas(const std::string &relative) const {
        return std::filesystem::exists(install / relative);
    }
};

} // namespace sfcop::hle::kernel::test
