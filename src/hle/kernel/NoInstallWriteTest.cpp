// The install is only ever read: no write row reaches it, and the five per-user folders are
// never looked up in it. Run through a Store that forwards to the host Store on real folders and
// records every call with the root it carried.

#include "hle/kernel/Kernel.h"
#include "hle/kernel/KernelTestFolders.h"
#include "hle/kernel/KernelTestStores.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace sfcop::hle::kernel {
namespace {

using test::RecordingStore;
using test::TestFolders;
using namespace win32;

// An install with files the game may try to change, a recording Store on it and a Kernel.
struct Recorded : TestFolders {
    RecordingStore *store = nullptr;
    std::unique_ptr<platform::Store> owned;
    std::unique_ptr<Kernel> kernel;

    Recorded() {
        std::filesystem::create_directories(user / "saves");
        for (const char *file : {"ClientSettings.gf", "Assets/Specs/rules.ini", "readme.txt",
                                 "Assets/a.dat", "sfc.ini"}) {
            test::GameFolders::Write(install / file, "install");
        }
        auto recording = std::make_unique<RecordingStore>(
            platform::makeStore(install.string(), user.string()));
        store = recording.get();
        owned = std::move(recording);
        kernel = std::make_unique<Kernel>(*owned);
    }
};

TEST_CASE("no write row reaches the install, only the per-user folder") {
    Recorded game;
    Kernel &kernel = *game.kernel;
    const char data[] = "changed";
    std::uint32_t written = 0;

    // CreateFileA/W with write access under every disposition, on an install file and a new one.
    for (std::uint32_t disposition :
         {kCreateNew, kCreateAlways, kOpenExisting, kOpenAlways, kTruncateExisting}) {
        for (const char *name : {"C:\\ClientSettings.gf", "C:\\Assets\\Specs\\rules.ini",
                                 "C:\\readme.txt", "C:\\fresh.gf", "C:\\Saves\\game.sav"}) {
            std::uint32_t file = kernel.CreateFile(name, kGenericRead | kGenericWrite, 0, disposition, 0);
            if (file != 0) {
                // WriteFile, SetFilePointer, SetEndOfFile, SetFileTime, FlushFileBuffers
                CHECK(kernel.WriteFile(file, data, sizeof data - 1, &written));
                kernel.SetFilePointer(file, 2, nullptr, kFileBegin);
                CHECK(kernel.SetEndOfFile(file));
                std::uint64_t time = 132000000000000000ULL;
                CHECK(kernel.SetFileTime(file, &time, &time, &time));
                CHECK(kernel.FlushFileBuffers(file));
                CHECK(kernel.CloseFile(file));
            }
        }
    }
    // Write access by the other access bits only.
    std::uint32_t file = kernel.CreateFile("C:\\Assets\\a.dat", kFileWriteData, 0, kOpenExisting, 0);
    REQUIRE(file != 0);
    CHECK(kernel.WriteFile(file, data, sizeof data - 1, &written));
    CHECK(kernel.CloseFile(file));

    // DeleteFileA, MoveFileA, CopyFile, CreateDirectoryA, RemoveDirectoryA, SetFileAttributesA.
    kernel.DeleteFile("C:\\ClientSettings.gf");
    kernel.DeleteFile("C:\\fresh.gf");
    kernel.MoveFile("C:\\Assets\\a.dat", "C:\\Assets\\b.dat");
    kernel.MoveFile("C:\\readme.txt", "C:\\Assets\\readme.txt");
    kernel.CopyFile("C:\\Assets\\Specs\\rules.ini", "C:\\rules.bak", false);
    kernel.CreateDirectory("C:\\Assets\\Made");
    kernel.CreateDirectory("C:\\Assets");
    kernel.CreateDirectory("C:\\Saves");
    kernel.RemoveDirectory("C:\\Assets\\Made");
    kernel.RemoveDirectory("C:\\Assets\\Specs");
    kernel.SetFileAttributes("C:\\ClientSettings.gf", 0x01);
    kernel.SetFileAttributes("C:\\Assets", 0x20);

    // WritePrivateProfileString: with a folder, bare, on an install file and a new one.
    CHECK(kernel.WritePrivateProfileString("S", "K", "V", "C:\\Assets\\Specs\\rules.ini"));
    CHECK(kernel.WritePrivateProfileString("S", "K", "V", "sfc.ini"));
    CHECK(kernel.WritePrivateProfileString("S", "K", "V", "C:\\new.ini"));
    CHECK(kernel.WritePrivateProfileString("S", nullptr, nullptr, "C:\\new.ini"));

    CHECK(game.store->violations == std::vector<std::string>{});
    CHECK(game.store->calls.size() > 100); // the rows really ran
    // The install still holds exactly what it did.
    CHECK(test::GameFolders::Read(game.install / "ClientSettings.gf") == "install");
    CHECK(test::GameFolders::Read(game.install / "Assets/Specs/rules.ini") == "install");
    CHECK(test::GameFolders::Read(game.install / "sfc.ini") == "install");
    CHECK(test::GameFolders::Read(game.install / "Assets/a.dat") == "install");
}

TEST_CASE("the recorder flags what would write the install") {
    Recorded game;
    CHECK(game.owned->Stat(platform::Root::kInstall, "sfc.ini"));
    CHECK(game.store->violations.empty());
    auto opened = game.owned->Open(platform::Root::kInstall, "sfc.ini", platform::OpenMode::kRead);
    REQUIRE(opened);
    (void)game.owned->Write(opened.Value(), "x", 1);
    (void)game.owned->Remove(platform::Root::kInstall, "sfc.ini");
    (void)game.owned->Open(platform::Root::kInstall, "sfc.ini", platform::OpenMode::kReadWrite);
    CHECK(game.store->violations.size() == 3);
}

TEST_CASE("a lookup under saves, replays, mods, settings or logs never touches the install") {
    for (const char *folder : {"saves", "replays", "mods", "settings", "logs", "Saves", "LOGS"}) {
        Recorded game;
        Kernel &kernel = *game.kernel;
        std::string base = std::string("C:\\") + folder;
        INFO(folder);
        CHECK(kernel.GetFileAttributes(base + "\\x.dat") == kInvalidFileAttributes);
        CHECK(kernel.GetFileAttributes(base + "\\Sub\\x.dat") == kInvalidFileAttributes);
        CHECK(kernel.CreateFile(base + "\\x.dat", kGenericRead, 0, kOpenExisting, 0) == 0);
        FindData first;
        CHECK(kernel.FindFirstFile(base + "\\x.*", &first) == 0);
        CHECK_FALSE(kernel.SetCurrentDirectory(base + "\\Sub"));
        CHECK(kernel.ShortPathName(base + "\\x.dat") == std::nullopt);
        kernel.CreateDirectory(base); // the folder itself, in the per-user root (it may exist)
        // Creating one there is the per-user folder's doing; and then it is found, still without
        // the install.
        std::uint32_t made = kernel.CreateFile(base + "\\x.dat", kGenericRead | kGenericWrite, 0, kOpenAlways, 0);
        CHECK(made != 0);
        CHECK(kernel.CloseFile(made));
        CHECK(kernel.GetFileAttributes(base + "\\x.dat") == kAttributeArchive);
        std::uint32_t found = kernel.FindFirstFile(base + "\\x.*", &first);
        CHECK(found != 0);
        CHECK(first.name == "x.dat");
        CHECK(game.store->installCalls == 0);
    }
}

} // namespace
} // namespace sfcop::hle::kernel
