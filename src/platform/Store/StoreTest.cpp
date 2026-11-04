#include "platform/Store/Store.h"
#include "platform/Store/StoreMock.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace sfcop::platform {
namespace {

constexpr const char *kTestKey = "HKEY_LOCAL_MACHINE\\Software\\TestCompany\\TestGame";

// A caller written against the interface only: reads a whole install file in 4-byte chunks.
Result<std::vector<std::uint8_t>> readWhole(Store &store, std::string_view path) {
    Result<FileHandle> file = store.Open(Root::kInstall, path, OpenMode::kRead);
    if (!file.Ok()) {
        return file.Code();
    }
    std::vector<std::uint8_t> content;
    std::uint8_t chunk[4];
    for (;;) {
        Result<std::size_t> got = store.Read(file.Value(), chunk, sizeof chunk);
        if (!got.Ok()) {
            (void)store.Close(file.Value());
            return got.Code();
        }
        if (got.Value() == 0) {
            break;
        }
        content.insert(content.end(), chunk, chunk + got.Value());
    }
    Status closed = store.Close(file.Value());
    if (!closed.Ok()) {
        return closed.Code();
    }
    return content;
}

// A caller written against the interface only: the game's "read a DWORD setting, default if
// missing" pattern.
std::uint32_t readDword(Store &store, std::string_view name, std::uint32_t fallback) {
    Result<RegistryValue> value = store.GetValue(kTestKey, name);
    if (!value.Ok() || value.Value().type != RegistryType::kDword) {
        return fallback;
    }
    return value.Value().dword;
}

} // namespace

TEST_CASE("a caller reads an install file to its end through the store interface") {
    StoreMock mock;
    mock.openResult = FileHandle{9};
    mock.readData = {1, 2, 3, 4, 5, 6};
    Result<std::vector<std::uint8_t>> content = readWhole(mock, "Assets/Test.txt");
    REQUIRE(content.Ok());
    CHECK(content.Value() == std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6});
    CHECK(mock.log.Calls() == std::vector<std::string>{
                                  "Open(0, \"Assets/Test.txt\", 0)",
                                  "Read(9, 4)",
                                  "Read(9, 4)",
                                  "Read(9, 4)",
                                  "Close(9)",
                              });
}

TEST_CASE("a missing install file and a failing read reach the caller as their errors") {
    StoreMock mock;
    mock.openResult = Error::kNotFound;
    CHECK(readWhole(mock, "Assets/Missing.txt").Code() == Error::kNotFound);
    mock.openResult = FileHandle{2};
    mock.readError = Error::kIoError;
    CHECK(readWhole(mock, "Assets/Test.txt").Code() == Error::kIoError);
    CHECK(mock.log.Count("Close") == 1);
}

TEST_CASE("a registry value reads back as set, and a missing one falls back") {
    StoreMock mock;
    Store &store = mock;
    RegistryValue width;
    width.type = RegistryType::kDword;
    width.dword = 1024;
    CHECK(store.SetValue(kTestKey, "ScreenWidth", width).Ok());
    CHECK(readDword(mock, "ScreenWidth", 640) == 1024);
    CHECK(readDword(mock, "ScreenHeight", 480) == 480);
    RegistryValue name;
    name.type = RegistryType::kString;
    name.text = "TestCaptain";
    CHECK(store.SetValue(kTestKey, "PlayerName", name).Ok());
    CHECK(readDword(mock, "PlayerName", 7) == 7);
    CHECK(mock.lastSetValue.text == "TestCaptain");
}

TEST_CASE("an atomic write hands the whole content to the store in one call") {
    StoreMock mock;
    Store &store = mock;
    const char content[] = "a=1\n";
    std::string path = std::string(userFolderName(UserFolder::kSettings)) + "/test.gf";
    CHECK(store.WriteAtomic(Root::kUser, path, content, 4).Ok());
    CHECK(mock.written == std::vector<std::uint8_t>{'a', '=', '1', '\n'});
    CHECK(mock.log.Calls() ==
          std::vector<std::string>{"WriteAtomic(1, \"settings/test.gf\", 4)"});
}

TEST_CASE("the user folders have the names of the design") {
    CHECK(std::string(userFolderName(UserFolder::kSettings)) == "settings");
    CHECK(std::string(userFolderName(UserFolder::kSaves)) == "saves");
    CHECK(std::string(userFolderName(UserFolder::kReplays)) == "replays");
    CHECK(std::string(userFolderName(UserFolder::kLogs)) == "logs");
    CHECK(std::string(userFolderName(UserFolder::kMods)) == "mods");
}

TEST_CASE("every method of the store stub reports unsupported") {
    std::unique_ptr<Store> store = makeStoreStub();
    FileHandle file{1};
    std::uint8_t buffer[4] = {};
    CHECK(store->RootPath(Root::kUser).Code() == Error::kUnsupported);
    CHECK(store->Open(Root::kInstall, "a.txt", OpenMode::kRead).Code() == Error::kUnsupported);
    CHECK(store->Read(file, buffer, 4).Code() == Error::kUnsupported);
    CHECK(store->Write(file, buffer, 4).Code() == Error::kUnsupported);
    CHECK(store->Seek(file, 0, SeekOrigin::kBegin).Code() == Error::kUnsupported);
    CHECK(store->GetInfo(file).Code() == Error::kUnsupported);
    CHECK(store->SetSize(file, 0).Code() == Error::kUnsupported);
    CHECK(store->SetModifiedTime(file, 0).Code() == Error::kUnsupported);
    CHECK(store->Flush(file).Code() == Error::kUnsupported);
    CHECK(store->Close(file).Code() == Error::kUnsupported);
    CHECK(store->Stat(Root::kUser, "a.txt").Code() == Error::kUnsupported);
    CHECK(store->List(Root::kUser, "").Code() == Error::kUnsupported);
    CHECK(store->MakeDirectory(Root::kUser, "saves").Code() == Error::kUnsupported);
    CHECK(store->Remove(Root::kUser, "a.txt").Code() == Error::kUnsupported);
    CHECK(store->Rename(Root::kUser, "a.txt", "b.txt").Code() == Error::kUnsupported);
    CHECK(store->WriteAtomic(Root::kUser, "a.txt", buffer, 4).Code() == Error::kUnsupported);
    CHECK(store->KeyExists(kTestKey).Code() == Error::kUnsupported);
    CHECK(store->CreateKey(kTestKey).Code() == Error::kUnsupported);
    CHECK(store->DeleteKey(kTestKey).Code() == Error::kUnsupported);
    CHECK(store->GetValue(kTestKey, "").Code() == Error::kUnsupported);
    CHECK(store->SetValue(kTestKey, "", RegistryValue{}).Code() == Error::kUnsupported);
}

} // namespace sfcop::platform
