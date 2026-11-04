// FindFirstFile and FindNextFile over the install and the per-user copies together.

#include "hle/kernel/Kernel.h"
#include "hle/kernel/KernelTestFolders.h"

#include <doctest/doctest.h>

#include <vector>

namespace sfcop::hle::kernel {
namespace {

using test::GameFolders;
using namespace win32;

std::vector<std::string> search(Kernel &kernel, const char *pattern) {
    std::vector<std::string> names;
    FindData data;
    std::uint32_t find = kernel.FindFirstFile(pattern, &data);
    if (find == 0) {
        return names;
    }
    do {
        names.push_back(data.name);
    } while (kernel.FindNextFile(find, &data));
    CHECK(kernel.TakeLastError() == kErrorNoMoreFiles);
    CHECK(kernel.FindClose(find));
    return names;
}

TEST_CASE("a search shows the install and the per-user copies together, in NTFS order") {
    GameFolders game;
    game.InstallFile("Assets/a.txt", "install");
    game.InstallFile("Assets/B.mod", "model");
    game.InstallFile("Assets/Sub/x.bmp", "");
    GameFolders::Write(game.user / "logs" / "Assets" / "a.txt", "mine, longer");
    GameFolders::Write(game.user / "logs" / "Assets" / "c.txt", "");
    GameFolders::Write(game.user / "settings" / "Assets" / "new.gf", "");
    GameFolders::Write(game.user / "settings" / "Assets" / ".new.gf.tmp3", "");
    GameFolders::Write(game.user / "settings" / "Assets" / ".keep.tmpl", ""); // not Store's pattern

    CHECK(search(game.kernel, "C:\\Assets\\*.*") ==
          std::vector<std::string>{".", "..", ".keep.tmpl", "a.txt", "B.mod", "c.txt", "new.gf",
                                   "Sub"});
    CHECK(search(game.kernel, "assets\\*.TXT") == std::vector<std::string>{"a.txt", "c.txt"});
    CHECK(search(game.kernel, "Assets\\?.txt") == std::vector<std::string>{"a.txt", "c.txt"});
    CHECK(search(game.kernel, "Assets\\*.") == std::vector<std::string>{".", "..", "Sub"});
    CHECK(search(game.kernel, "Assets\\A.TXT") == std::vector<std::string>{"a.txt"});

    FindData data;
    std::uint32_t find = game.kernel.FindFirstFile("Assets\\a.txt", &data);
    REQUIRE(find != 0);
    CHECK(data.size == 12); // the per-user copy
    CHECK(data.attributes == kAttributeArchive);
    CHECK(data.writeTime == data.creationTime);
    CHECK(data.writeTime > 116444736000000000ULL);
    CHECK(game.kernel.FindClose(find));
    find = game.kernel.FindFirstFile("Assets\\Sub", &data);
    REQUIRE(find != 0);
    CHECK(data.attributes == kAttributeDirectory);
    CHECK(data.size == 0);
    CHECK(game.kernel.FindClose(find));
}

TEST_CASE("the game folder lists no dot entries, its settings copies and the user's save folders") {
    GameFolders game;
    game.InstallFile("ClientSettings.gf", "install");
    game.InstallFile("StarFleetOP.exe", "");
    GameFolders::Write(game.user / "settings" / "ClientSettings.gf", "mine");
    GameFolders::Write(game.user / "logs" / "QAReport.txt", "");
    std::filesystem::create_directories(game.user / "saves");
    GameFolders::Write(game.user / "registry.tsv", "");

    CHECK(search(game.kernel, "C:\\*.*") ==
          std::vector<std::string>{"ClientSettings.gf", "QAReport.txt", "saves", "StarFleetOP.exe"});
    CHECK(search(game.kernel, "*.gf") == std::vector<std::string>{"ClientSettings.gf"});
    CHECK(search(game.kernel, "C:*.exe") == std::vector<std::string>{"StarFleetOP.exe"});
    CHECK(search(game.kernel, "saves\\*.*") == std::vector<std::string>{".", ".."});
}

TEST_CASE("a search that finds nothing, a missing folder and an empty mask fail as on Windows") {
    GameFolders game;
    game.InstallFile("Assets/a.txt", "");
    FindData data;
    CHECK(game.kernel.FindFirstFile("Assets\\*.bmp", &data) == 0);
    CHECK(game.kernel.TakeLastError() == kErrorFileNotFound);
    CHECK(game.kernel.FindFirstFile("Missing\\*.*", &data) == 0);
    CHECK(game.kernel.TakeLastError() == kErrorPathNotFound);
    CHECK(game.kernel.FindFirstFile("D:\\*.*", &data) == 0);
    CHECK(game.kernel.TakeLastError() == kErrorPathNotFound);
    CHECK(game.kernel.FindFirstFile("Assets\\", &data) == 0);
    CHECK(game.kernel.TakeLastError() == kErrorFileNotFound);
    CHECK(game.kernel.FindFirstFile("Assets\\a|b", &data) == 0);
    CHECK(game.kernel.TakeLastError() == kErrorInvalidName);
}

TEST_CASE("a search handle ends with ERROR_NO_MORE_FILES and closes once") {
    GameFolders game;
    game.InstallFile("Assets/a.txt", "");
    FindData data;
    std::uint32_t find = game.kernel.FindFirstFile("Assets\\a.txt", &data);
    REQUIRE(find != 0);
    CHECK(game.kernel.IsFind(find));
    CHECK_FALSE(game.kernel.FindNextFile(find, &data));
    CHECK(game.kernel.TakeLastError() == kErrorNoMoreFiles);
    CHECK(game.kernel.FindClose(find));
    CHECK_FALSE(game.kernel.FindClose(find));
    CHECK(game.kernel.TakeLastError() == kErrorInvalidHandle);
    CHECK_FALSE(game.kernel.FindNextFile(find, &data));
    CHECK(game.kernel.TakeLastError() == kErrorInvalidHandle);
}

} // namespace
} // namespace sfcop::hle::kernel
