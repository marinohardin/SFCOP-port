// CreateFile and the calls on an open file, as the game makes them; the write redirect into the
// per-user folder; deleting, moving, copying, directories and attributes.

#include "hle/kernel/Kernel.h"
#include "hle/kernel/KernelTestFolders.h"
#include "platform/Store/StoreMock.h"

#include <doctest/doctest.h>

#include <vector>

namespace sfcop::hle::kernel {
namespace {

using test::GameFolders;
using namespace win32;

// A settings file as the game writes one: text and binary bytes, CR LF line ends.
const std::string kGfFile = std::string("[Video]\r\nWidth=1024\r\nHeight=768\r\n[Bytes]\r\n") +
                            std::string("\x00\x01\xfe\xff", 4) + "\r\n";

std::string readThrough(Kernel &kernel, const char *name) {
    std::uint32_t file = kernel.CreateFile(name, kGenericRead, 1, kOpenExisting, 0x80);
    REQUIRE(file != 0);
    std::string content;
    char chunk[5];
    std::uint32_t got = 0;
    do {
        REQUIRE(kernel.ReadFile(file, chunk, sizeof chunk, &got));
        content.append(chunk, got);
    } while (got == sizeof chunk);
    CHECK(kernel.CloseFile(file));
    return content;
}

void writeThrough(Kernel &kernel, const char *name, const std::string &content,
                  std::uint32_t disposition) {
    std::uint32_t file = kernel.CreateFile(name, kGenericWrite, 0, disposition, 0x80);
    REQUIRE(file != 0);
    std::uint32_t written = 0;
    CHECK(kernel.WriteFile(file, content.data(), static_cast<std::uint32_t>(content.size()), &written));
    CHECK(written == content.size());
    CHECK(kernel.CloseFile(file));
}

TEST_CASE("a .gf file written through WriteFile reads back byte for byte through ReadFile") {
    GameFolders game;
    writeThrough(game.kernel, "C:\\TestProfile.gf", kGfFile, kCreateAlways);
    CHECK(readThrough(game.kernel, "c:\\testprofile.GF") == kGfFile);
    CHECK(game.UserContent("settings/TestProfile.gf") == kGfFile);
    CHECK_FALSE(game.InstallHas("TestProfile.gf"));
}

TEST_CASE("rewriting the install's settings file goes to the per-user copy and leaves the install") {
    GameFolders game;
    const std::string original = "[Video]\r\nWidth=640\r\n";
    game.InstallFile("ClientSettings.gf", original);
    game.InstallFile("MetaAssets/ClientSettings/Help.gf", "help");
    CHECK(readThrough(game.kernel, "ClientSettings.gf") == original);

    writeThrough(game.kernel, "C:\\ClientSettings.gf", kGfFile, kCreateAlways);
    CHECK(game.InstallContent("ClientSettings.gf") == original);
    CHECK(game.UserContent("settings/ClientSettings.gf") == kGfFile);
    CHECK(readThrough(game.kernel, "C:\\ClientSettings.gf") == kGfFile);

    // A file opened to change in place is copied first, so its old bytes are there to read.
    std::uint32_t file =
        game.kernel.CreateFile("MetaAssets\\ClientSettings\\Help.gf", kGenericRead | kGenericWrite,
                               0, kOpenExisting, 0);
    REQUIRE(file != 0);
    CHECK(game.kernel.SetFilePointer(file, 0, nullptr, kFileEnd) == 4);
    CHECK(game.kernel.WriteFile(file, "!", 1, nullptr));
    CHECK(game.kernel.SetFilePointer(file, 0, nullptr, kFileBegin) == 0);
    char back[8] = {};
    std::uint32_t got = 0;
    CHECK(game.kernel.ReadFile(file, back, sizeof back, &got));
    CHECK(std::string(back, got) == "help!");
    CHECK(game.kernel.CloseFile(file));
    CHECK(game.InstallContent("MetaAssets/ClientSettings/Help.gf") == "help");
    CHECK(game.UserContent("settings/MetaAssets/ClientSettings/Help.gf") == "help!");
}

TEST_CASE("the game's logs and saves land in their own per-user folders") {
    GameFolders game;
    game.InstallFile("Assets/Specs/shiplist.txt", "ships");
    writeThrough(game.kernel, "C:\\QAReport.txt", "report", kCreateAlways);
    writeThrough(game.kernel, "Assets\\Specs\\shiplist.txt", "edited", kTruncateExisting);
    CHECK(game.kernel.CreateDirectory("C:\\saves"));
    writeThrough(game.kernel, "C:\\Saves\\Campaign One.sav", "save", kCreateNew);
    CHECK(game.UserContent("logs/QAReport.txt") == "report");
    CHECK(game.UserContent("logs/Assets/Specs/shiplist.txt") == "edited");
    CHECK(game.InstallContent("Assets/Specs/shiplist.txt") == "ships");
    CHECK(game.UserContent("saves/Campaign One.sav") == "save");
    CHECK(readThrough(game.kernel, "saves\\campaign one.SAV") == "save");
}

TEST_CASE("CreateFile follows each creation disposition and sets the last error as Windows does") {
    GameFolders game;
    game.InstallFile("Assets/Specs/rules.ini", "rules");
    Kernel &kernel = game.kernel;

    CHECK(kernel.CreateFile("Assets\\Specs\\rules.ini", kGenericWrite, 0, kCreateNew, 0) == 0);
    CHECK(kernel.TakeLastError() == kErrorFileExists);

    std::uint32_t file = kernel.CreateFile("Assets\\Specs\\rules.ini", kGenericWrite, 0, kOpenAlways, 0);
    REQUIRE(file != 0);
    CHECK(kernel.TakeLastError() == kErrorAlreadyExists);
    CHECK(kernel.CloseFile(file));
    CHECK(game.UserContent("settings/Assets/Specs/rules.ini") == "rules");

    file = kernel.CreateFile("Assets\\Specs\\new.ini", kGenericRead, 0, kOpenAlways, 0);
    REQUIRE(file != 0);
    CHECK(kernel.TakeLastError() == 0u);
    CHECK(kernel.CloseFile(file));
    CHECK(game.UserHas("settings/Assets/Specs/new.ini"));

    file = kernel.CreateFile("Assets\\Specs\\rules.ini", kGenericWrite, 0, kCreateAlways, 0);
    REQUIRE(file != 0);
    CHECK(kernel.TakeLastError() == kErrorAlreadyExists);
    CHECK(kernel.GetFileSize(file, nullptr) == 0);
    CHECK(kernel.CloseFile(file));

    CHECK(kernel.CreateFile("Assets\\Specs\\none.ini", kGenericRead, 0, kOpenExisting, 0) == 0);
    CHECK(kernel.TakeLastError() == kErrorFileNotFound);
    CHECK(kernel.CreateFile("Assets\\None\\none.ini", kGenericWrite, 0, kCreateAlways, 0) == 0);
    CHECK(kernel.TakeLastError() == kErrorPathNotFound);
    CHECK(kernel.CreateFile("Assets\\Specs\\none.ini", kGenericWrite, 0, kTruncateExisting, 0) == 0);
    CHECK(kernel.TakeLastError() == kErrorFileNotFound);
    CHECK(kernel.CreateFile("Assets\\Specs\\rules.ini", kGenericRead, 0, kTruncateExisting, 0) == 0);
    CHECK(kernel.TakeLastError() == kErrorInvalidParameter);
    CHECK(kernel.CreateFile("Assets\\Specs\\rules.ini", kGenericRead, 0, 6, 0) == 0);
    CHECK(kernel.TakeLastError() == kErrorInvalidParameter);
    CHECK(kernel.CreateFile("Assets", kGenericRead, 0, kOpenExisting, 0) == 0);
    CHECK(kernel.TakeLastError() == kErrorAccessDenied);
    CHECK(kernel.CreateFile("D:\\rules.ini", kGenericRead, 0, kOpenExisting, 0) == 0);
    CHECK(kernel.TakeLastError() == kErrorPathNotFound);
    CHECK(kernel.CreateFile("rul*s.ini", kGenericRead, 0, kOpenExisting, 0) == 0);
    CHECK(kernel.TakeLastError() == kErrorInvalidName);
    CHECK(game.InstallContent("Assets/Specs/rules.ini") == "rules");
}

TEST_CASE("a handle reads, writes, seeks and sizes only as it was opened") {
    GameFolders game;
    game.InstallFile("data.bin", "0123456789");
    Kernel &kernel = game.kernel;
    std::uint32_t reader = kernel.CreateFile("data.bin", kGenericRead, 1, kOpenExisting, 0);
    REQUIRE(reader != 0);
    CHECK_FALSE(kernel.WriteFile(reader, "x", 1, nullptr));
    CHECK(kernel.TakeLastError() == kErrorAccessDenied);
    CHECK_FALSE(kernel.SetEndOfFile(reader));
    CHECK(kernel.TakeLastError() == kErrorAccessDenied);
    CHECK_FALSE(kernel.FlushFileBuffers(reader));
    CHECK(kernel.TakeLastError() == kErrorAccessDenied);

    std::uint32_t high = 7;
    CHECK(kernel.GetFileSize(reader, &high) == 10);
    CHECK(high == 0);
    CHECK(kernel.TakeLastError() == 0u);
    CHECK(kernel.SetFilePointer(reader, 4, nullptr, kFileBegin) == 4);
    CHECK(kernel.SetFilePointer(reader, 2, nullptr, kFileCurrent) == 6);
    CHECK(kernel.SetFilePointer(reader, -3, nullptr, kFileEnd) == 7);
    char three[3];
    std::uint32_t got = 0;
    CHECK(kernel.ReadFile(reader, three, 3, &got));
    CHECK(std::string(three, got) == "789");
    CHECK(kernel.ReadFile(reader, three, 3, &got));
    CHECK(got == 0);
    std::int32_t moveHigh = 0;
    CHECK(kernel.SetFilePointer(reader, 1, &moveHigh, kFileBegin) == 1);
    CHECK(moveHigh == 0);
    CHECK(kernel.SetFilePointer(reader, -20, nullptr, kFileCurrent) == kInvalidSetFilePointer);
    CHECK(kernel.TakeLastError() == kErrorNegativeSeek);
    CHECK(kernel.SetFilePointer(reader, 0, nullptr, 3) == kInvalidSetFilePointer);
    CHECK(kernel.TakeLastError() == kErrorInvalidParameter);
    CHECK(kernel.CloseFile(reader));

    std::uint32_t writer = kernel.CreateFile("data.bin", kGenericWrite, 0, kOpenExisting, 0);
    REQUIRE(writer != 0);
    CHECK_FALSE(kernel.ReadFile(writer, three, 3, &got));
    CHECK(kernel.TakeLastError() == kErrorAccessDenied);
    CHECK(got == 0);
    CHECK(kernel.SetFilePointer(writer, 4, nullptr, kFileBegin) == 4);
    CHECK(kernel.SetEndOfFile(writer));
    CHECK(kernel.FlushFileBuffers(writer));
    CHECK(kernel.WriteFile(writer, "", 0, nullptr));
    CHECK(kernel.GetFileSize(writer, nullptr) == 4);
    CHECK(kernel.CloseFile(writer));
    CHECK(game.UserContent("settings/data.bin") == "0123");
    CHECK(game.InstallContent("data.bin") == "0123456789");
}

// The calls a model loader makes on a disk file it only reads (SetFilePointer and ReadFile in the
// Win32 reference): each handle has its own file pointer; a read that crosses the end returns TRUE
// with the bytes up to the end; moving the pointer past the end succeeds and returns the new
// position, and a read there returns TRUE with 0 bytes, the end-of-file answer, not an error.
TEST_CASE("a read-only disk file: positions per handle, reads short at the end, seeks past it") {
    GameFolders game;
    game.InstallFile("Assets/Models/test/test.mod", "MDLS0123456789");
    Kernel &kernel = game.kernel;
    std::uint32_t first = kernel.CreateFile("C:\\Assets\\Models\\test\\test.mod", kGenericRead, 1,
                                            kOpenExisting, 0x80);
    std::uint32_t second = kernel.CreateFile("C:\\Assets\\Models\\test\\test.mod", kGenericRead, 1,
                                             kOpenExisting, 0x80);
    REQUIRE(first != 0);
    REQUIRE(second != 0);
    REQUIRE(first != second);

    char four[4];
    std::uint32_t got = 0;
    CHECK(kernel.ReadFile(first, four, 4, &got));
    CHECK(std::string(four, got) == "MDLS");
    CHECK(kernel.SetFilePointer(second, 10, nullptr, kFileBegin) == 10);
    CHECK(kernel.ReadFile(first, four, 4, &got)); // the second handle's move did not move this one
    CHECK(std::string(four, got) == "0123");
    CHECK(kernel.SetFilePointer(first, 0, nullptr, kFileCurrent) == 8);

    char eight[8];
    CHECK(kernel.ReadFile(second, eight, 8, &got));
    CHECK(got == 4);
    CHECK(std::string(eight, got) == "6789");
    CHECK(kernel.SetFilePointer(second, 0, nullptr, kFileCurrent) == 14);

    CHECK(kernel.SetFilePointer(first, 1044270900, nullptr, kFileBegin) == 1044270900u);
    CHECK(kernel.TakeLastError() == 0u);
    got = 7;
    CHECK(kernel.ReadFile(first, four, 4, &got));
    CHECK(got == 0);
    CHECK(kernel.SetFilePointer(first, 0, nullptr, kFileCurrent) == 1044270900u);
    CHECK(kernel.GetFileSize(first, nullptr) == 14);
    CHECK(kernel.CloseFile(first));
    CHECK(kernel.CloseFile(second));
}

TEST_CASE("a file's time is set and read as a FILETIME") {
    GameFolders game;
    Kernel &kernel = game.kernel;
    std::uint32_t file = kernel.CreateFile("C:\\timed.gf", kGenericRead | kGenericWrite, 0,
                                           kCreateAlways, 0);
    REQUIRE(file != 0);
    const std::uint64_t written = 126444736000000000ULL; // 2001-09-09 01:46:40 UTC
    CHECK(kernel.SetFileTime(file, nullptr, nullptr, &written));
    std::uint64_t creation = 0;
    std::uint64_t access = 0;
    std::uint64_t write = 0;
    CHECK(kernel.GetFileTime(file, &creation, &access, &write));
    CHECK(write == written);
    CHECK(creation == written);
    CHECK(access == written);
    CHECK(kernel.SetFileTime(file, nullptr, nullptr, nullptr));
    CHECK(kernel.CloseFile(file));
}

TEST_CASE("a closed or unknown handle is refused with ERROR_INVALID_HANDLE") {
    GameFolders game;
    Kernel &kernel = game.kernel;
    std::uint32_t file = kernel.CreateFile("C:\\a.gf", kGenericWrite, 0, kCreateAlways, 0);
    REQUIRE(file != 0);
    CHECK(kernel.IsFile(file));
    CHECK(kernel.CloseFile(file));
    CHECK_FALSE(kernel.IsFile(file));
    char byte = 0;
    std::uint32_t count = 5;
    std::uint64_t time = 0;
    CHECK_FALSE(kernel.CloseFile(file));
    CHECK(kernel.TakeLastError() == kErrorInvalidHandle);
    CHECK_FALSE(kernel.ReadFile(file, &byte, 1, &count));
    CHECK(count == 0);
    CHECK(kernel.TakeLastError() == kErrorInvalidHandle);
    CHECK_FALSE(kernel.WriteFile(file, &byte, 1, &count));
    CHECK(kernel.TakeLastError() == kErrorInvalidHandle);
    CHECK(kernel.SetFilePointer(file, 0, nullptr, kFileBegin) == kInvalidSetFilePointer);
    CHECK(kernel.TakeLastError() == kErrorInvalidHandle);
    CHECK(kernel.GetFileSize(file, nullptr) == kInvalidFileSize);
    CHECK(kernel.TakeLastError() == kErrorInvalidHandle);
    CHECK_FALSE(kernel.GetFileTime(file, &time, &time, &time));
    CHECK_FALSE(kernel.SetFileTime(file, &time, &time, &time));
    CHECK_FALSE(kernel.FlushFileBuffers(file));
    CHECK_FALSE(kernel.SetEndOfFile(file));
    CHECK(kernel.TakeLastError() == kErrorInvalidHandle);
}

TEST_CASE("deleting removes the per-user copy only; the install's file cannot be deleted") {
    GameFolders game;
    game.InstallFile("ClientSettings.gf", "install");
    Kernel &kernel = game.kernel;
    writeThrough(kernel, "ClientSettings.gf", "mine", kCreateAlways);
    writeThrough(kernel, "C:\\Scratch.gf", "scratch", kCreateAlways);
    CHECK(kernel.DeleteFile("C:\\scratch.gf"));
    CHECK_FALSE(game.UserHas("settings/Scratch.gf"));
    CHECK(kernel.DeleteFile("ClientSettings.gf"));
    CHECK(readThrough(kernel, "ClientSettings.gf") == "install");
    CHECK_FALSE(kernel.DeleteFile("ClientSettings.gf"));
    CHECK(kernel.TakeLastError() == kErrorAccessDenied);
    CHECK_FALSE(kernel.DeleteFile("Scratch.gf"));
    CHECK(kernel.TakeLastError() == kErrorFileNotFound);
    CHECK(game.InstallContent("ClientSettings.gf") == "install");
}

TEST_CASE("moving and copying work on the per-user copies") {
    GameFolders game;
    game.InstallFile("Assets/Specs/rules.ini", "rules");
    Kernel &kernel = game.kernel;
    writeThrough(kernel, "C:\\old.gf", "content", kCreateAlways);
    CHECK(kernel.MoveFile("C:\\old.gf", "C:\\new.gf"));
    CHECK(readThrough(kernel, "new.gf") == "content");
    CHECK_FALSE(game.UserHas("settings/old.gf"));
    writeThrough(kernel, "C:\\other.gf", "x", kCreateAlways);
    CHECK_FALSE(kernel.MoveFile("other.gf", "new.gf"));
    CHECK(kernel.TakeLastError() == kErrorAlreadyExists);
    CHECK_FALSE(kernel.MoveFile("Assets\\Specs\\rules.ini", "rules.ini"));
    CHECK(kernel.TakeLastError() == kErrorAccessDenied);
    CHECK_FALSE(kernel.MoveFile("absent.gf", "x.gf"));
    CHECK(kernel.TakeLastError() == kErrorFileNotFound);
    // The target's folder must exist, as on Windows: nothing is made for it.
    CHECK_FALSE(kernel.MoveFile("new.gf", "C:\\NoFolder\\moved.gf"));
    CHECK(kernel.TakeLastError() == kErrorPathNotFound);
    CHECK(readThrough(kernel, "new.gf") == "content");
    CHECK_FALSE(game.UserHas("settings/NoFolder"));

    CHECK(kernel.CopyFile("Assets\\Specs\\rules.ini", "C:\\rules.bak", true));
    CHECK(game.UserContent("settings/rules.bak") == "rules");
    CHECK_FALSE(kernel.CopyFile("Assets\\Specs\\rules.ini", "C:\\rules.bak", true));
    CHECK(kernel.TakeLastError() == kErrorFileExists);
    CHECK(kernel.CopyFile("new.gf", "C:\\rules.bak", false));
    CHECK(game.UserContent("settings/rules.bak") == "content");
    CHECK_FALSE(kernel.CopyFile("absent.gf", "x.gf", false));
    CHECK(kernel.TakeLastError() == kErrorFileNotFound);
}

TEST_CASE("directories are made in the per-user folder and removed only when the game made them") {
    GameFolders game;
    game.InstallFile("Assets/Specs/rules.ini", "rules");
    Kernel &kernel = game.kernel;
    CHECK(kernel.CreateDirectory("C:\\Assets\\NewFolder"));
    CHECK(game.UserHas("settings/Assets/NewFolder"));
    CHECK(kernel.GetFileAttributes("Assets\\NewFolder") == kAttributeDirectory);
    CHECK_FALSE(kernel.CreateDirectory("C:\\Assets\\NewFolder"));
    CHECK(kernel.TakeLastError() == kErrorAlreadyExists);
    CHECK_FALSE(kernel.CreateDirectory("C:\\Assets"));
    CHECK(kernel.TakeLastError() == kErrorAlreadyExists);
    CHECK_FALSE(kernel.CreateDirectory("C:\\None\\Deeper"));
    CHECK(kernel.TakeLastError() == kErrorPathNotFound);

    writeThrough(kernel, "Assets\\NewFolder\\a.txt", "log", kCreateAlways);
    CHECK_FALSE(kernel.RemoveDirectory("Assets\\NewFolder"));
    CHECK(kernel.TakeLastError() == kErrorDirNotEmpty);
    CHECK(kernel.DeleteFile("Assets\\NewFolder\\a.txt"));
    CHECK(kernel.RemoveDirectory("Assets\\NewFolder"));
    CHECK_FALSE(game.UserHas("settings/Assets/NewFolder"));
    CHECK_FALSE(game.UserHas("logs/Assets/NewFolder"));
    CHECK_FALSE(kernel.RemoveDirectory("Assets\\NewFolder"));
    CHECK(kernel.TakeLastError() == kErrorFileNotFound);
    CHECK_FALSE(kernel.RemoveDirectory("Assets\\Specs"));
    CHECK(kernel.TakeLastError() == kErrorAccessDenied);
    CHECK_FALSE(kernel.RemoveDirectory("Assets\\Specs\\rules.ini"));
    CHECK(kernel.TakeLastError() == kErrorDirectory);
}

TEST_CASE("attributes: a directory, a writable file, or INVALID_FILE_ATTRIBUTES") {
    GameFolders game;
    game.InstallFile("Assets/Specs/rules.ini", "rules");
    Kernel &kernel = game.kernel;
    CHECK(kernel.GetFileAttributes("C:\\") == kAttributeDirectory);
    CHECK(kernel.GetFileAttributes("assets") == kAttributeDirectory);
    CHECK(kernel.GetFileAttributes("Assets\\Specs\\RULES.INI") == kAttributeArchive);
    CHECK(kernel.GetFileAttributes("Assets\\Specs\\none.ini") == kInvalidFileAttributes);
    CHECK(kernel.TakeLastError() == kErrorFileNotFound);
    CHECK(kernel.GetFileAttributes("Assets\\None\\none.ini") == kInvalidFileAttributes);
    CHECK(kernel.TakeLastError() == kErrorPathNotFound);
    CHECK(kernel.SetFileAttributes("Assets\\Specs\\rules.ini", kAttributeReadOnly));
    CHECK(kernel.GetFileAttributes("Assets\\Specs\\rules.ini") == kAttributeArchive);
    CHECK_FALSE(kernel.SetFileAttributes("Assets\\Specs\\none.ini", 0));
    CHECK(kernel.TakeLastError() == kErrorFileNotFound);
}

TEST_CASE("a Store failure reaches the game as the Win32 error for it") {
    platform::StoreMock store;
    Kernel kernel(store);
    store.statResult = platform::FileInfo{};
    store.openResult = platform::Error::kOutOfResources;
    CHECK(kernel.CreateFile("C:\\a.gf", kGenericRead, 0, kOpenExisting, 0) == 0);
    CHECK(kernel.TakeLastError() == kErrorTooManyOpenFiles);
    CHECK(store.log.Calls() == std::vector<std::string>{
                                   "Stat(1, \"settings/a.gf\")",
                                   "Open(1, \"settings/a.gf\", 0)",
                               });

    store.openResult = platform::FileHandle{4};
    std::uint32_t file = kernel.CreateFile("C:\\a.gf", kGenericRead | kGenericWrite, 0, kOpenExisting, 0);
    REQUIRE(file != 0);
    store.readError = platform::Error::kIoError;
    char byte = 0;
    CHECK_FALSE(kernel.ReadFile(file, &byte, 1, nullptr));
    CHECK(kernel.TakeLastError() == kErrorReadFault);
    store.writeError = platform::Error::kIoError;
    CHECK_FALSE(kernel.WriteFile(file, &byte, 1, nullptr));
    CHECK(kernel.TakeLastError() == kErrorWriteFault);
    store.closeResult = platform::Error::kIoError;
    CHECK_FALSE(kernel.CloseFile(file));
    CHECK(kernel.TakeLastError() == kErrorWriteFault);
    CHECK(win32Error(platform::Error::kUnsupported, false) == kErrorNotSupported);
    CHECK(win32Error(platform::Error::kTimedOut, false) == kErrorGenFailure);
}

TEST_CASE("a read error while copying an install file to its per-user path stays a read error") {
    platform::StoreMock store;
    Kernel kernel(store);
    store.readError = platform::Error::kIoError;
    CHECK_FALSE(kernel.CopyFile("C:\\a.gf", "C:\\b.gf", false));
    CHECK(kernel.TakeLastError() == kErrorReadFault);
}

} // namespace
} // namespace sfcop::hle::kernel
