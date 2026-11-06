// The per-user logs folder placed elsewhere (LogsStore.h), through the Kernel and through the
// bridge as the guest host attaches it.

#include "hle/kernel/KernelBridge.h"
#include "hle/kernel/KernelTestFolders.h"
#include "hle/kernel/LogsStore.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace sfcop::hle::kernel {
namespace {

using namespace win32;
using platform::Root;

// An install, a per-user folder and a logs folder, with a Kernel on the LogsStore over them.
struct LogsFolders : test::TestFolders {
    std::filesystem::path logs;
    std::unique_ptr<platform::Store> store;
    Kernel kernel;

    LogsFolders()
        : logs(base / "elsewhere"),
          store((std::filesystem::create_directories(logs),
                 std::make_unique<LogsStore>(platform::makeStore(install.string(), user.string()),
                                             platform::makeStore(install.string(), logs.string())))),
          kernel(*store) {}

    std::uint32_t WriteGameFile(const char *name, const std::string &content) {
        const std::uint32_t file = kernel.CreateFile(name, kGenericWrite, 0, kCreateAlways, 0);
        REQUIRE(file != 0);
        std::uint32_t written = 0;
        REQUIRE(kernel.WriteFile(file, content.data(), static_cast<std::uint32_t>(content.size()),
                                 &written));
        REQUIRE(kernel.CloseFile(file));
        return file;
    }
};

std::vector<std::string> names(const std::vector<platform::DirectoryEntry> &entries) {
    std::vector<std::string> out;
    for (const platform::DirectoryEntry &entry : entries) {
        out.push_back(entry.name);
    }
    return out;
}

} // namespace

TEST_CASE("a path under logs is routed to the logs folder, in any case, and nothing else is") {
    std::string_view rest;
    CHECK(LogsStore::IsLogsPath("logs", &rest));
    CHECK(rest.empty());
    CHECK(LogsStore::IsLogsPath("logs/OracleTrace.txt", &rest));
    CHECK(rest == "OracleTrace.txt");
    CHECK(LogsStore::IsLogsPath("LOGS/Assets/x.txt", &rest));
    CHECK(rest == "Assets/x.txt");
    CHECK_FALSE(LogsStore::IsLogsPath("logs2/x.txt", nullptr));
    CHECK_FALSE(LogsStore::IsLogsPath("settings/logs/x.txt", nullptr));
    CHECK_FALSE(LogsStore::IsLogsPath("", nullptr));
}

TEST_CASE("a .txt the game writes beside the exe lands in the logs folder, not the per-user one") {
    LogsFolders folders;
    folders.WriteGameFile("C:\\OracleTrace.txt", "# oracle-trace v1\nEND 0 aborted\n");
    CHECK(test::GameFolders::Read(folders.logs / "OracleTrace.txt") ==
          "# oracle-trace v1\nEND 0 aborted\n");
    CHECK_FALSE(std::filesystem::exists(folders.user / "logs" / "OracleTrace.txt"));
    CHECK_FALSE(std::filesystem::exists(folders.user / "logs"));

    // The game reads it back through the same path, and sees it in a directory search.
    const std::uint32_t file =
        folders.kernel.CreateFile("C:\\OracleTrace.txt", kGenericRead, 0, kOpenExisting, 0);
    REQUIRE(file != 0);
    char buffer[64] = {};
    std::uint32_t read = 0;
    REQUIRE(folders.kernel.ReadFile(file, buffer, sizeof buffer, &read));
    CHECK(std::string(buffer, read) == "# oracle-trace v1\nEND 0 aborted\n");
    CHECK(folders.kernel.CloseFile(file));
    FindData found;
    const std::uint32_t search = folders.kernel.FindFirstFile("C:\\*.txt", &found);
    REQUIRE(search != 0);
    CHECK(found.name == "OracleTrace.txt");
    CHECK(folders.kernel.FindClose(search));
}

TEST_CASE("a settings file still lands in the per-user folder, and the registry stays there") {
    LogsFolders folders;
    folders.WriteGameFile("C:\\ClientSettings.gf", "gf");
    CHECK(test::GameFolders::Read(folders.user / "settings" / "ClientSettings.gf") == "gf");
    CHECK_FALSE(std::filesystem::exists(folders.logs / "settings"));
    REQUIRE(folders.store->CreateKey("HKEY_LOCAL_MACHINE\\Software\\TestCompany").Ok());
    CHECK(std::filesystem::exists(folders.user / "registry.tsv"));
    CHECK_FALSE(std::filesystem::exists(folders.logs / "registry.tsv"));
}

TEST_CASE("the logs folder itself is a directory that can be listed and not removed") {
    LogsFolders folders;
    folders.WriteGameFile("C:\\QAReport.txt", "q");
    platform::Result<platform::FileInfo> info = folders.store->Stat(Root::kUser, "logs");
    REQUIRE(info.Ok());
    CHECK(info.Value().isDirectory);
    CHECK(folders.store->MakeDirectory(Root::kUser, "logs").Ok());
    CHECK(folders.store->Remove(Root::kUser, "logs").Code() == platform::Error::kAccessDenied);
    platform::Result<std::vector<platform::DirectoryEntry>> listed =
        folders.store->List(Root::kUser, "logs");
    REQUIRE(listed.Ok());
    CHECK(names(listed.Value()) == std::vector<std::string>{"QAReport.txt"});
    CHECK(folders.store->Rename(Root::kUser, "logs/QAReport.txt", "settings/QAReport.txt").Code() ==
          platform::Error::kAccessDenied);
    CHECK(folders.store->Rename(Root::kUser, "logs/QAReport.txt", "logs/Report.txt").Ok());
    CHECK(std::filesystem::exists(folders.logs / "Report.txt"));
}

TEST_CASE("handles of the two sides are told apart, and a closed one is dead") {
    LogsFolders folders;
    folders.WriteGameFile("C:\\a.txt", "log side");
    folders.WriteGameFile("C:\\a.gf", "user side");
    platform::Result<platform::FileHandle> log =
        folders.store->Open(Root::kUser, "logs/a.txt", platform::OpenMode::kRead);
    platform::Result<platform::FileHandle> user =
        folders.store->Open(Root::kUser, "settings/a.gf", platform::OpenMode::kRead);
    REQUIRE(log.Ok());
    REQUIRE(user.Ok());
    CHECK(log.Value() != user.Value());
    char buffer[16] = {};
    platform::Result<std::size_t> got = folders.store->Read(user.Value(), buffer, sizeof buffer);
    REQUIRE(got.Ok());
    CHECK(std::string(buffer, got.Value()) == "user side");
    got = folders.store->Read(log.Value(), buffer, sizeof buffer);
    REQUIRE(got.Ok());
    CHECK(std::string(buffer, got.Value()) == "log side");
    CHECK(folders.store->Close(log.Value()).Ok());
    CHECK(folders.store->Close(log.Value()).Code() == platform::Error::kInvalidHandle);
    CHECK(folders.store->Read(log.Value(), buffer, 1).Code() == platform::Error::kInvalidHandle);
    CHECK(folders.store->Close(user.Value()).Ok());
}

TEST_CASE("the bridge attached with a logs folder writes the game's .txt there") {
    test::TestFolders folders;
    const std::filesystem::path logs = folders.base / "logs-elsewhere";
    sfcop_hle_kernel_attach_logs(folders.install.string().c_str(), folders.user.string().c_str(),
                                 logs.string().c_str());
    REQUIRE(sfcop_hle_kernel_attached() == 1);
    const std::uint32_t file =
        sfcop_kernel_create_file("C:\\OracleTrace.txt", 0, kGenericWrite, 0, kCreateAlways, 0);
    REQUIRE(file != 0);
    std::uint32_t written = 0;
    REQUIRE(sfcop_kernel_write_file(file, "END 3 max_samples\n", 18, &written) == 1);
    REQUIRE(sfcop_kernel_close_file(file) == 1);
    sfcop_hle_kernel_detach();
    CHECK(test::GameFolders::Read(logs / "OracleTrace.txt") == "END 3 max_samples\n");
    CHECK_FALSE(std::filesystem::exists(folders.user / "logs" / "OracleTrace.txt"));

    // Without a logs folder the attachment is the plain one: the file goes under the per-user logs/.
    sfcop_hle_kernel_attach_logs(folders.install.string().c_str(), folders.user.string().c_str(),
                                 nullptr);
    const std::uint32_t plain =
        sfcop_kernel_create_file("C:\\Other.txt", 0, kGenericWrite, 0, kCreateAlways, 0);
    REQUIRE(plain != 0);
    REQUIRE(sfcop_kernel_close_file(plain) == 1);
    sfcop_hle_kernel_detach();
    CHECK(std::filesystem::exists(folders.user / "logs" / "Other.txt"));
    CHECK_FALSE(std::filesystem::exists(logs / "Other.txt"));
}

} // namespace sfcop::hle::kernel
