// The game's path model: Win32 names to game paths, the per-user copy of each, the current
// directory and the calls that answer names, drives and volumes.

#include "hle/kernel/Kernel.h"
#include "hle/kernel/KernelTestFolders.h"
#include "hle/kernel/Text.h"
#include "platform/Store/StoreMock.h"

#include <doctest/doctest.h>

namespace sfcop::hle::kernel {
namespace {

using test::GameFolders;

TEST_CASE("a Win32 name is made absolute against the current directory") {
    platform::StoreMock store;
    Kernel kernel(store);
    CHECK(kernel.Absolute("Assets\\Specs\\shiplist.txt") == "C:\\Assets\\Specs\\shiplist.txt");
    CHECK(kernel.Absolute("assets/specs/x.txt") == "C:\\assets\\specs\\x.txt");
    CHECK(kernel.Absolute("\\SFC.INI") == "C:\\SFC.INI");
    CHECK(kernel.Absolute("c:\\a\\..\\b\\.\\c") == "C:\\b\\c");
    CHECK(kernel.Absolute("a\\\\b\\") == "C:\\a\\b");
    CHECK(kernel.Absolute("name. ") == "C:\\name");
    CHECK(kernel.Absolute("..\\..") == "C:\\");
    CHECK(kernel.Absolute("D:\\Data") == "D:\\Data");
    CHECK(kernel.Absolute("D:Data") == "D:\\Data");
    CHECK(kernel.Absolute("") == "");
    CHECK(kernel.Absolute("\\\\server\\share\\x") == "");
    CHECK(kernel.Absolute("a*b") == "");
    CHECK(kernel.Absolute("ab:c") == "");
}

TEST_CASE("a path under the game folder is a game path; anything else is not") {
    platform::StoreMock store;
    KernelConfig config;
    config.gameFolder = "C:\\Games\\SFC";
    Kernel kernel(store, config);
    CHECK(kernel.CurrentDirectory() == "C:\\Games\\SFC");
    CHECK(kernel.ModuleFileName() == "C:\\Games\\SFC\\StarFleetOP.exe");
    GamePath inside = kernel.Locate("c:\\games\\sfc\\Assets\\Specs\\shiplist.txt");
    CHECK(inside.inGame);
    CHECK(inside.relative == "Assets/Specs/shiplist.txt");
    GamePath folder = kernel.Locate("C:\\Games\\SFC");
    CHECK(folder.inGame);
    CHECK(folder.relative == "");
    CHECK(kernel.Locate("ClientSettings.gf").relative == "ClientSettings.gf");
    CHECK_FALSE(kernel.Locate("C:\\Games\\SFCX\\a.txt").inGame);
    CHECK_FALSE(kernel.Locate("C:\\WINDOWS\\SFC.INI").inGame);
    CHECK_FALSE(kernel.Locate("D:\\Games\\SFC\\a.txt").inGame);
}

TEST_CASE("a game path's per-user copy goes to saves, replays, mods, logs or settings") {
    CHECK(Kernel::UserPathOf("ClientSettings.gf") == "settings/ClientSettings.gf");
    CHECK(Kernel::UserPathOf("MetaAssets/ClientSettings/Test.gf") ==
          "settings/MetaAssets/ClientSettings/Test.gf");
    CHECK(Kernel::UserPathOf("SFC.INI") == "settings/SFC.INI");
    CHECK(Kernel::UserPathOf("QAReport.txt") == "logs/QAReport.txt");
    CHECK(Kernel::UserPathOf("Assets/debug.LOG") == "logs/Assets/debug.LOG");
    CHECK(Kernel::UserPathOf("Saves/Campaign One.sav") == "saves/Campaign One.sav");
    CHECK(Kernel::UserPathOf("replays") == "replays");
    CHECK(Kernel::UserPathOf("mods/TestMod/a.txt") == "mods/TestMod/a.txt");
    CHECK(Kernel::UserPathOf("settings/a.gf") == "settings/a.gf");
    CHECK(Kernel::UserPathOf("Settings/windows/a.ini") == "settings/windows/a.ini");
    CHECK(Kernel::UserPathOf("logs/a.txt") == "logs/a.txt");
    CHECK(Kernel::UserPathOf("LOGS/Sub/a.dat") == "logs/Sub/a.dat");
    CHECK(Kernel::UserPathOf("settingsX/a.gf") == "settings/settingsX/a.gf");
    CHECK(Kernel::UserPathOf("Assets") == "settings/Assets");
}

TEST_CASE("the current directory changes only to a folder the game can see") {
    GameFolders game;
    game.InstallFile("Assets/Specs/shiplist.txt", "x");
    Kernel &kernel = game.kernel;
    CHECK(kernel.CurrentDirectory() == "C:\\");
    CHECK(kernel.SetCurrentDirectory("Assets"));
    CHECK(kernel.CurrentDirectory() == "C:\\Assets");
    CHECK(kernel.Locate("Specs\\shiplist.txt").relative == "Assets/Specs/shiplist.txt");
    CHECK(kernel.SetCurrentDirectory("..\\ASSETS\\specs\\"));
    CHECK(kernel.CurrentDirectory() == "C:\\ASSETS\\specs");

    CHECK_FALSE(kernel.SetCurrentDirectory("C:\\Missing"));
    CHECK(kernel.TakeLastError() == win32::kErrorFileNotFound);
    CHECK_FALSE(kernel.SetCurrentDirectory("C:\\Missing\\Deeper"));
    CHECK(kernel.TakeLastError() == win32::kErrorPathNotFound);
    CHECK_FALSE(kernel.SetCurrentDirectory("shiplist.txt"));
    CHECK(kernel.TakeLastError() == win32::kErrorDirectory);
    CHECK_FALSE(kernel.SetCurrentDirectory("D:\\"));
    CHECK(kernel.TakeLastError() == win32::kErrorPathNotFound);
    CHECK(kernel.CurrentDirectory() == "C:\\ASSETS\\specs");
    CHECK_FALSE(kernel.TakeLastError().has_value());
}

TEST_CASE("the full path name keeps a trailing separator and points at the last segment") {
    platform::StoreMock store;
    Kernel kernel(store);
    std::size_t part = 0;
    CHECK(kernel.FullPathName("Assets\\a.txt", &part) == std::optional<std::string>("C:\\Assets\\a.txt"));
    CHECK(part == 10);
    CHECK(kernel.FullPathName("C:\\Assets\\", &part) == std::optional<std::string>("C:\\Assets\\"));
    CHECK(part == std::string::npos);
    CHECK(kernel.FullPathName("C:\\", &part) == std::optional<std::string>("C:\\"));
    CHECK(part == std::string::npos);
    CHECK_FALSE(kernel.FullPathName("\\\\server\\x", &part).has_value());
    CHECK(kernel.TakeLastError() == win32::kErrorInvalidName);
}

TEST_CASE("the short path name of a file that exists is its own name") {
    GameFolders game;
    game.InstallFile("Assets/Long Name File.txt", "x");
    CHECK(game.kernel.ShortPathName("C:\\Assets\\Long Name File.txt") ==
          std::optional<std::string>("C:\\Assets\\Long Name File.txt"));
    CHECK_FALSE(game.kernel.ShortPathName("C:\\Assets\\none.txt").has_value());
    CHECK(game.kernel.TakeLastError() == win32::kErrorFileNotFound);
    CHECK_FALSE(game.kernel.ShortPathName("C:\\None\\none.txt").has_value());
    CHECK(game.kernel.TakeLastError() == win32::kErrorPathNotFound);
}

TEST_CASE("the game's drive is a fixed disk with room; every other drive is absent") {
    platform::StoreMock store;
    Kernel kernel(store);
    CHECK(kernel.DriveType("C:\\") == win32::kDriveFixed);
    CHECK(kernel.DriveType("c:") == win32::kDriveFixed);
    CHECK(kernel.DriveType("") == win32::kDriveFixed);
    CHECK(kernel.DriveType("D:\\") == win32::kDriveNoRootDir);
    CHECK(kernel.DriveType("1:\\") == win32::kDriveNoRootDir);

    std::uint32_t sectors = 0;
    std::uint32_t bytes = 0;
    std::uint32_t free = 0;
    std::uint32_t total = 0;
    CHECK(kernel.DiskFreeSpace("C:\\", &sectors, &bytes, &free, &total));
    CHECK(sectors == 8);
    CHECK(bytes == 512);
    CHECK(free == 0x7FFFF);
    CHECK(total == 0xFFFFF);
    CHECK_FALSE(kernel.DiskFreeSpace("E:\\", &sectors, &bytes, &free, &total));
    CHECK(kernel.TakeLastError() == win32::kErrorPathNotFound);

    std::optional<Kernel::VolumeInfo> volume = kernel.VolumeInformation("C:\\");
    REQUIRE(volume.has_value());
    CHECK(volume->name == "SFCOP");
    CHECK(volume->serial == 0x5FC00255u);
    CHECK(volume->maxComponentLength == 255);
    CHECK(volume->flags == 6);
    CHECK(volume->fileSystem == "NTFS");
    CHECK_FALSE(kernel.VolumeInformation("D:\\").has_value());
    CHECK(store.log.Calls().empty());
}

TEST_CASE("a FILETIME and Store's nanoseconds convert both ways") {
    CHECK(fileTimeFromNs(0) == 116444736000000000ULL);
    CHECK(fileTimeFromNs(1000000000000000000LL) == 126444736000000000ULL);
    CHECK(nsFromFileTime(126444736000000000ULL) == 1000000000000000000LL);
}

TEST_CASE("the game's strings convert between ANSI, UTF-16 and UTF-8") {
    CHECK(ansiToUtf8("Caf\xe9") == "Caf\xc3\xa9");
    CHECK(utf8ToAnsi("Caf\xc3\xa9") == "Caf\xe9");
    CHECK(utf8ToAnsi("\xe2\x82\xac") == "?");
    CHECK(utf8ToAnsi("\xff") == "?");
    CHECK(utf16ToUtf8(u"Caf\u00e9 \U0001F680") == "Caf\xc3\xa9 \xf0\x9f\x9a\x80");
    CHECK(utf8ToUtf16("Caf\xc3\xa9 \xf0\x9f\x9a\x80") == u"Caf\u00e9 \U0001F680");
    CHECK(utf16ToUtf8(std::u16string(1, char16_t(0xD800))) == "\xef\xbf\xbd");
    const std::uint16_t wide[] = {'a', 'b', 0};
    CHECK(wideFrom(wide) == u"ab");
    CHECK(wideFrom(nullptr).empty());
}

TEST_CASE("a string is answered into a buffer only whole, else the size it needs comes back") {
    char buffer[8] = "xxxxxxx";
    CHECK(answerString("C:\\abc", buffer, 8) == 6);
    CHECK(std::string(buffer) == "C:\\abc");
    CHECK(answerString("C:\\abcdefg", buffer, 8) == 11);
    CHECK(std::string(buffer) == "C:\\abc");
    CHECK(answerString("C:\\", nullptr, 0) == 4);
    std::uint16_t wide[4] = {9, 9, 9, 9};
    CHECK(answerWide(u"C:\\", wide, 4) == 3);
    CHECK(wide[3] == 0);
    CHECK(answerWide(u"C:\\x", wide, 4) == 5);
    char cut[4];
    CHECK(copyTruncated("abcdef", cut, 4) == 3);
    CHECK(std::string(cut) == "abc");
    CHECK(copyTruncated("abcdef", cut, 0) == 0);
    std::uint16_t wideCut[3];
    CHECK(copyTruncatedWide(u"abcdef", wideCut, 3) == 2);
    CHECK(wideCut[2] == 0);
}

} // namespace
} // namespace sfcop::hle::kernel
