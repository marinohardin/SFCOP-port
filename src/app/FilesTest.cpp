#include "app/Files.h"

#include <filesystem>
#include <memory>

#include <doctest/doctest.h>

#include "app/FilesMock.h"

using sfcop::app::FileReader;
using sfcop::app::hostFileSystem;
using sfcop::app::joinPath;

namespace {

// A fresh folder under the system's temporary folder, removed afterwards.
class ScratchDir {
public:
    explicit ScratchDir(const char *name)
        : path_(std::filesystem::temp_directory_path() / name) {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    ~ScratchDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    std::string str() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

} // namespace

TEST_CASE("paths join with the separator the folder already uses") {
    CHECK(joinPath("/games/op", "Assets/a.mod") == "/games/op/Assets/a.mod");
    CHECK(joinPath("/games/op/", "a.mod") == "/games/op/a.mod");
    CHECK(joinPath("", "a.mod") == "a.mod");
    CHECK(joinPath("C:\\Games\\OP", "Assets/a.mod") == "C:\\Games\\OP\\Assets\\a.mod");
    CHECK(joinPath("C:\\Games\\OP\\", "a.mod") == "C:\\Games\\OP\\a.mod");
    CHECK(joinPath("C:/Games\\OP", "Assets/a.mod") == "C:/Games\\OP/Assets/a.mod");
}

TEST_CASE("the host file system creates folders and replaces a file atomically") {
    const ScratchDir scratch("sfcop-c4-files-test");
    const std::string dir = joinPath(scratch.str(), "a/b");
    REQUIRE(hostFileSystem().createDirectories(dir));
    CHECK(hostFileSystem().isDirectory(dir));
    CHECK(hostFileSystem().createDirectories(dir));

    const std::string file = joinPath(dir, "install.txt");
    REQUIRE(hostFileSystem().writeTextAtomic(file, "first\n"));
    REQUIRE(hostFileSystem().writeTextAtomic(file, "second\n"));
    CHECK(hostFileSystem().readText(file) == std::optional<std::string>("second\n"));
    CHECK(hostFileSystem().fileSize(file) == std::optional<std::uint64_t>(7));
    CHECK_FALSE(hostFileSystem().fileSize(file + ".tmp").has_value());
    CHECK_FALSE(hostFileSystem().fileSize(dir).has_value());
}

TEST_CASE("the host file system makes a relative path absolute against the current folder") {
    CHECK(hostFileSystem().absolutePath("/games/op") == "/games/op");
    const std::string here = std::filesystem::current_path().lexically_normal().u8string();
    CHECK(hostFileSystem().absolutePath("op") == here + "/op");
    CHECK(hostFileSystem().absolutePath("x/../op/") == here + "/op");
}

TEST_CASE("the host file system reads a fixture file to its end in pieces") {
    const std::string path = sfcop::app::testing::fixturesDir() + "/install/Sounds/hail.wav";
    const std::unique_ptr<FileReader> reader = hostFileSystem().openRead(path);
    REQUIRE(reader);
    unsigned char piece[10];
    std::string got;
    while (const std::size_t n = reader->read(piece, sizeof piece)) {
        got.append(reinterpret_cast<const char *>(piece), n);
    }
    CHECK_FALSE(reader->failed());
    CHECK(got == "RIFF invented sound stand-in");
    CHECK(hostFileSystem().openRead(path + ".absent") == nullptr);
}
