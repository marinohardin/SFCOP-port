// The host Store on a directory the test creates and removes: every method's success and the
// errors Store.h lists, the lookup without case, the atomic write and registry.tsv.

#include "platform/Store/Store.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace sfcop::platform {
namespace {

namespace fs = std::filesystem;

// An install folder and a per-user folder under a fresh temporary directory, removed afterwards.
struct Folders {
    fs::path base;
    fs::path install;
    fs::path user;

    Folders() {
        base = fs::temp_directory_path() /
               ("sfcop-storehost-test-" + std::to_string(std::random_device{}()));
        install = base / "install";
        user = base / "user";
        fs::create_directories(install);
        fs::create_directories(user);
    }
    ~Folders() {
        std::error_code ignored;
        fs::permissions(user / "locked", fs::perms::owner_all, ignored);
        fs::remove_all(base, ignored);
    }
    Folders(const Folders &) = delete;
    Folders &operator=(const Folders &) = delete;

    std::unique_ptr<Store> open() const { return makeStore(install.string(), user.string()); }
};

void writeHostFile(const fs::path &path, const std::string &content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << content;
}

std::string readHostFile(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream content;
    content << in.rdbuf();
    return content.str();
}

std::string readAll(Store &store, FileHandle file) {
    std::string out;
    char chunk[3];
    for (;;) {
        Result<std::size_t> got = store.Read(file, chunk, sizeof chunk);
        REQUIRE(got.Ok());
        if (got.Value() == 0) {
            return out;
        }
        out.append(chunk, got.Value());
    }
}

std::vector<std::string> names(const std::vector<DirectoryEntry> &entries) {
    std::vector<std::string> out;
    for (const DirectoryEntry &entry : entries) {
        out.push_back(entry.name);
    }
    return out;
}

TEST_CASE("the host store answers each root's path, and kNotFound for a root it was not given") {
    Folders folders;
    auto store = folders.open();
    CHECK(store->RootPath(Root::kInstall).Value() == folders.install.string());
    CHECK(store->RootPath(Root::kUser).Value() == folders.user.string());

    auto bare = makeStore("", "");
    CHECK(bare->RootPath(Root::kInstall).Code() == Error::kNotFound);
    CHECK(bare->Open(Root::kUser, "a.txt", OpenMode::kCreate).Code() == Error::kNotFound);
    CHECK(bare->CreateKey("HKEY_CURRENT_USER\\Software\\TestCompany").Code() == Error::kIoError);
    CHECK_FALSE(bare->KeyExists("HKEY_CURRENT_USER\\Software\\TestCompany").Value());
}

TEST_CASE("a user file is written, sought, sized, timed and read back") {
    Folders folders;
    auto store = folders.open();
    Result<FileHandle> file = store->Open(Root::kUser, "notes.txt", OpenMode::kCreate);
    REQUIRE(file.Ok());
    CHECK(store->Write(file.Value(), "abcdef", 6).Value() == 6);
    CHECK(store->Seek(file.Value(), 2, SeekOrigin::kBegin).Value() == 2);
    CHECK(readAll(*store, file.Value()) == "cdef");
    CHECK(store->Seek(file.Value(), -1, SeekOrigin::kEnd).Value() == 5);
    CHECK(store->Seek(file.Value(), -2, SeekOrigin::kCurrent).Value() == 3);
    CHECK(store->Seek(file.Value(), -4, SeekOrigin::kCurrent).Code() == Error::kInvalidArgument);

    // A write past the end fills the gap with zeros.
    CHECK(store->Seek(file.Value(), 8, SeekOrigin::kBegin).Value() == 8);
    CHECK(store->Write(file.Value(), "Z", 1).Value() == 1);
    FileInfo info = store->GetInfo(file.Value()).Value();
    CHECK(info.size == 9);
    CHECK_FALSE(info.isDirectory);
    CHECK_FALSE(info.readOnly);

    CHECK(store->SetSize(file.Value(), 4).Ok());
    CHECK(store->GetInfo(file.Value()).Value().size == 4);
    CHECK(store->SetModifiedTime(file.Value(), 1000000000000000000LL).Ok());
    CHECK(store->GetInfo(file.Value()).Value().modifiedNs == 1000000000000000000LL);
    CHECK(store->Flush(file.Value()).Ok());
    CHECK(store->Close(file.Value()).Ok());
    CHECK(readHostFile(folders.user / "notes.txt") == std::string("abcd"));

    FileInfo stat = store->Stat(Root::kUser, "notes.txt").Value();
    CHECK(stat.size == 4);
    CHECK(stat.modifiedNs == 1000000000000000000LL);
}

TEST_CASE("the open modes keep, truncate or refuse existing content as Store.h says") {
    Folders folders;
    writeHostFile(folders.user / "keep.gf", "old");
    auto store = folders.open();

    Result<FileHandle> kept = store->Open(Root::kUser, "keep.gf", OpenMode::kOpenOrCreate);
    REQUIRE(kept.Ok());
    CHECK(readAll(*store, kept.Value()) == "old");
    CHECK(store->Close(kept.Value()).Ok());

    Result<FileHandle> both = store->Open(Root::kUser, "keep.gf", OpenMode::kReadWrite);
    REQUIRE(both.Ok());
    CHECK(store->Write(both.Value(), "n", 1).Ok());
    CHECK(store->Close(both.Value()).Ok());
    CHECK(readHostFile(folders.user / "keep.gf") == "nld");

    CHECK(store->Open(Root::kUser, "keep.gf", OpenMode::kCreateNew).Code() ==
          Error::kAlreadyExists);
    Result<FileHandle> fresh = store->Open(Root::kUser, "keep.gf", OpenMode::kCreate);
    REQUIRE(fresh.Ok());
    CHECK(store->GetInfo(fresh.Value()).Value().size == 0);
    CHECK(store->Close(fresh.Value()).Ok());

    Result<FileHandle> created = store->Open(Root::kUser, "new.gf", OpenMode::kOpenOrCreate);
    REQUIRE(created.Ok());
    CHECK(store->Close(created.Value()).Ok());
    CHECK(fs::exists(folders.user / "new.gf"));
}

TEST_CASE("opening reports a missing file, a missing parent and a directory") {
    Folders folders;
    fs::create_directories(folders.user / "saves");
    auto store = folders.open();
    CHECK(store->Open(Root::kUser, "absent.gf", OpenMode::kRead).Code() == Error::kNotFound);
    CHECK(store->Open(Root::kUser, "absent.gf", OpenMode::kReadWrite).Code() == Error::kNotFound);
    CHECK(store->Open(Root::kUser, "nowhere/a.gf", OpenMode::kCreate).Code() == Error::kNotFound);
    CHECK(store->Open(Root::kUser, "saves", OpenMode::kRead).Code() == Error::kWrongState);
    CHECK(store->Open(Root::kUser, "saves", OpenMode::kCreate).Code() == Error::kWrongState);
}

TEST_CASE("a malformed path is refused by every path call") {
    Folders folders;
    auto store = folders.open();
    const char *const bad[] = {"/abs.txt", "a//b", "a/./b", "../up", "a\\b", "C:x", "trailing/"};
    for (const char *path : bad) {
        CAPTURE(path);
        CHECK(store->Open(Root::kUser, path, OpenMode::kCreate).Code() == Error::kInvalidArgument);
        CHECK(store->Stat(Root::kUser, path).Code() == Error::kInvalidArgument);
        CHECK(store->List(Root::kUser, path).Code() == Error::kInvalidArgument);
        CHECK(store->MakeDirectory(Root::kUser, path).Code() == Error::kInvalidArgument);
        CHECK(store->Remove(Root::kUser, path).Code() == Error::kInvalidArgument);
        CHECK(store->Rename(Root::kUser, path, "x").Code() == Error::kInvalidArgument);
        CHECK(store->WriteAtomic(Root::kUser, path, "x", 1).Code() == Error::kInvalidArgument);
    }
    CHECK(store->Open(Root::kUser, "", OpenMode::kRead).Code() == Error::kInvalidArgument);
    CHECK(store->Stat(Root::kUser, "").Code() == Error::kInvalidArgument);
}

TEST_CASE("the install is read-only: every write mode and change is refused") {
    Folders folders;
    writeHostFile(folders.install / "Assets" / "Specs" / "rules.ini", "[a]\r\nb=1\r\n");
    auto store = folders.open();
    for (OpenMode mode : {OpenMode::kReadWrite, OpenMode::kCreate, OpenMode::kCreateNew,
                          OpenMode::kOpenOrCreate}) {
        CHECK(store->Open(Root::kInstall, "Assets/Specs/rules.ini", mode).Code() ==
              Error::kAccessDenied);
    }
    CHECK(store->MakeDirectory(Root::kInstall, "New").Code() == Error::kAccessDenied);
    CHECK(store->Remove(Root::kInstall, "Assets/Specs/rules.ini").Code() == Error::kAccessDenied);
    CHECK(store->Rename(Root::kInstall, "Assets", "Other").Code() == Error::kAccessDenied);
    CHECK(store->WriteAtomic(Root::kInstall, "x.gf", "x", 1).Code() == Error::kAccessDenied);

    Result<FileHandle> file = store->Open(Root::kInstall, "Assets/Specs/rules.ini", OpenMode::kRead);
    REQUIRE(file.Ok());
    CHECK(store->GetInfo(file.Value()).Value().readOnly);
    CHECK(store->Write(file.Value(), "x", 1).Code() == Error::kAccessDenied);
    CHECK(store->SetSize(file.Value(), 0).Code() == Error::kAccessDenied);
    CHECK(store->SetModifiedTime(file.Value(), 0).Code() == Error::kAccessDenied);
    CHECK(store->Flush(file.Value()).Ok());
    CHECK(store->Close(file.Value()).Ok());
    CHECK(store->Stat(Root::kInstall, "Assets/Specs/rules.ini").Value().readOnly);
    CHECK(readHostFile(folders.install / "Assets" / "Specs" / "rules.ini") == "[a]\r\nb=1\r\n");
}

TEST_CASE("a path is found whatever its ASCII case, and a created file keeps the case it was given") {
    Folders folders;
    writeHostFile(folders.install / "Assets" / "Specs" / "Rules.ini", "rules");
    auto store = folders.open();
    Result<FileHandle> file = store->Open(Root::kInstall, "ASSETS/specs/rules.INI", OpenMode::kRead);
    REQUIRE(file.Ok());
    CHECK(readAll(*store, file.Value()) == "rules");
    CHECK(store->Close(file.Value()).Ok());
    // A second lookup comes from the cache and agrees.
    CHECK(store->Stat(Root::kInstall, "assets/SPECS/RULES.ini").Value().size == 5);
    CHECK(names(store->List(Root::kInstall, "aSSets").Value()) ==
          std::vector<std::string>{"Specs"});

    REQUIRE(store->MakeDirectory(Root::kUser, "Saves").Ok());
    Result<FileHandle> created = store->Open(Root::kUser, "SAVES/Game One.SAV", OpenMode::kCreate);
    REQUIRE(created.Ok());
    CHECK(store->Close(created.Value()).Ok());
    CHECK(names(store->List(Root::kUser, "saves").Value()) ==
          std::vector<std::string>{"Game One.SAV"});
}

TEST_CASE("a directory lists its entries in byte order, without the dot entries") {
    Folders folders;
    writeHostFile(folders.install / "gamma.txt", "123");
    writeHostFile(folders.install / "beta.txt", "");
    fs::create_directories(folders.install / "Alpha");
    auto store = folders.open();
    std::vector<DirectoryEntry> entries = store->List(Root::kInstall, "").Value();
    CHECK(names(entries) == std::vector<std::string>{"Alpha", "beta.txt", "gamma.txt"});
    CHECK(entries[0].info.isDirectory);
    CHECK(entries[0].info.size == 0);
    CHECK(entries[2].info.size == 3);
    CHECK(entries[2].info.readOnly);
    CHECK(store->List(Root::kInstall, "gamma.txt").Code() == Error::kWrongState);
    CHECK(store->List(Root::kInstall, "missing").Code() == Error::kNotFound);
    CHECK(store->Stat(Root::kInstall, "missing").Code() == Error::kNotFound);
}

TEST_CASE("directories are made with their parents and removed only when empty") {
    Folders folders;
    auto store = folders.open();
    CHECK(store->MakeDirectory(Root::kUser, "saves/campaign/one").Ok());
    CHECK(store->MakeDirectory(Root::kUser, "saves/campaign").Ok());
    CHECK(store->Stat(Root::kUser, "saves/campaign/one").Value().isDirectory);
    REQUIRE(store->WriteAtomic(Root::kUser, "saves/campaign/one/a.sav", "a", 1).Ok());
    CHECK(store->MakeDirectory(Root::kUser, "saves/campaign/one/a.sav").Code() ==
          Error::kAlreadyExists);

    CHECK(store->Remove(Root::kUser, "saves/campaign/one").Code() == Error::kWrongState);
    Result<FileHandle> open = store->Open(Root::kUser, "saves/campaign/one/a.sav", OpenMode::kRead);
    REQUIRE(open.Ok());
    CHECK(store->Remove(Root::kUser, "saves/campaign/one/a.sav").Code() == Error::kAccessDenied);
    CHECK(store->Close(open.Value()).Ok());
    CHECK(store->Remove(Root::kUser, "SAVES/campaign/one/A.SAV").Ok());
    CHECK(store->Remove(Root::kUser, "saves/campaign/one").Ok());
    CHECK(store->Remove(Root::kUser, "saves/campaign/one").Code() == Error::kNotFound);
    CHECK_FALSE(fs::exists(folders.user / "saves" / "campaign" / "one"));
}

TEST_CASE("a rename replaces a file but not a directory, and needs its source and target parent") {
    Folders folders;
    auto store = folders.open();
    REQUIRE(store->WriteAtomic(Root::kUser, "a.gf", "new", 3).Ok());
    REQUIRE(store->WriteAtomic(Root::kUser, "b.gf", "old", 3).Ok());
    REQUIRE(store->MakeDirectory(Root::kUser, "logs").Ok());
    CHECK(store->Rename(Root::kUser, "a.gf", "b.gf").Ok());
    CHECK(readHostFile(folders.user / "b.gf") == "new");
    CHECK_FALSE(fs::exists(folders.user / "a.gf"));
    CHECK(store->Rename(Root::kUser, "b.gf", "logs").Code() == Error::kAlreadyExists);
    CHECK(store->Rename(Root::kUser, "a.gf", "c.gf").Code() == Error::kNotFound);
    CHECK(store->Rename(Root::kUser, "b.gf", "nowhere/c.gf").Code() == Error::kNotFound);
    CHECK(store->Rename(Root::kUser, "B.GF", "logs/b.gf").Ok());
    CHECK(readHostFile(folders.user / "logs" / "b.gf") == "new");
    CHECK(store->Rename(Root::kUser, "logs/b.gf", "logs/B.gf").Ok());
    CHECK(names(store->List(Root::kUser, "logs").Value()) == std::vector<std::string>{"B.gf"});
}

TEST_CASE("an atomic write replaces the content whole, or leaves the old content") {
    Folders folders;
    auto store = folders.open();
    REQUIRE(store->MakeDirectory(Root::kUser, "settings").Ok());
    CHECK(store->WriteAtomic(Root::kUser, "settings/ClientSettings.gf", "first", 5).Ok());
    CHECK(store->WriteAtomic(Root::kUser, "SETTINGS/clientsettings.gf", "second", 6).Ok());
    CHECK(readHostFile(folders.user / "settings" / "ClientSettings.gf") == "second");
    CHECK(names(store->List(Root::kUser, "settings").Value()) ==
          std::vector<std::string>{"ClientSettings.gf"});
    CHECK(store->WriteAtomic(Root::kUser, "settings/empty.gf", "", 0).Ok());
    CHECK(store->Stat(Root::kUser, "settings/empty.gf").Value().size == 0);

    // A write whose parent is missing fails before anything is written.
    CHECK(store->WriteAtomic(Root::kUser, "missing/ClientSettings.gf", "x", 1).Code() ==
          Error::kNotFound);
    CHECK_FALSE(fs::exists(folders.user / "missing"));

    // A write cut short (here: the temporary file cannot be made) keeps the old content and
    // leaves nothing behind.
    REQUIRE(store->MakeDirectory(Root::kUser, "locked").Ok());
    REQUIRE(store->WriteAtomic(Root::kUser, "locked/kept.gf", "old", 3).Ok());
    fs::permissions(folders.user / "locked", fs::perms::owner_read | fs::perms::owner_exec);
    std::ofstream probe(folders.user / "locked" / "probe");
    const bool lockHolds = !probe.is_open();
    probe.close();
    if (lockHolds) {
        CHECK(store->WriteAtomic(Root::kUser, "locked/kept.gf", "new", 3).Code() ==
              Error::kIoError);
        CHECK(readHostFile(folders.user / "locked" / "kept.gf") == "old");
        CHECK(names(store->List(Root::kUser, "locked").Value()) ==
              std::vector<std::string>{"kept.gf"});
    } else {
        MESSAGE("skipped: this user can write into a read-only directory");
    }
}

TEST_CASE("a handle that is not open is refused by every handle call") {
    Folders folders;
    auto store = folders.open();
    Result<FileHandle> file = store->Open(Root::kUser, "a.txt", OpenMode::kCreate);
    REQUIRE(file.Ok());
    CHECK(store->Close(file.Value()).Ok());
    char byte = 0;
    for (FileHandle dead : {file.Value(), FileHandle{}, FileHandle{999}}) {
        CHECK(store->Read(dead, &byte, 1).Code() == Error::kInvalidHandle);
        CHECK(store->Write(dead, &byte, 1).Code() == Error::kInvalidHandle);
        CHECK(store->Seek(dead, 0, SeekOrigin::kBegin).Code() == Error::kInvalidHandle);
        CHECK(store->GetInfo(dead).Code() == Error::kInvalidHandle);
        CHECK(store->SetSize(dead, 0).Code() == Error::kInvalidHandle);
        CHECK(store->SetModifiedTime(dead, 0).Code() == Error::kInvalidHandle);
        CHECK(store->Flush(dead).Code() == Error::kInvalidHandle);
        CHECK(store->Close(dead).Code() == Error::kInvalidHandle);
    }
}

constexpr const char *kGameKey = "HKEY_LOCAL_MACHINE\\SOFTWARE\\TestCompany\\TestGame";

TEST_CASE("registry keys are created with their parents and compared without case") {
    Folders folders;
    auto store = folders.open();
    CHECK_FALSE(store->KeyExists(kGameKey).Value());
    CHECK(store->KeyExists("HKEY_LOCAL_MACHINE").Value());
    CHECK(store->CreateKey(kGameKey).Ok());
    CHECK(store->CreateKey(kGameKey).Ok());
    CHECK(store->KeyExists("hkey_local_machine\\software\\testcompany").Value());
    CHECK(store->KeyExists("HKEY_LOCAL_MACHINE\\SOFTWARE\\TESTCOMPANY\\TESTGAME").Value());
    for (const char *bad : {"", "\\Lead", "Trail\\", "A\\\\B", "Tab\tKey"}) {
        CAPTURE(bad);
        CHECK(store->KeyExists(bad).Code() == Error::kInvalidArgument);
        CHECK(store->CreateKey(bad).Code() == Error::kInvalidArgument);
    }
}

TEST_CASE("registry values of the three types are set, replaced and read back") {
    Folders folders;
    auto store = folders.open();
    REQUIRE(store->CreateKey(kGameKey).Ok());

    RegistryValue number;
    number.dword = 0x12345678;
    RegistryValue text;
    text.type = RegistryType::kString;
    text.text = "C:\\Games\\Test";
    RegistryValue bytes;
    bytes.type = RegistryType::kBinary;
    bytes.bytes = {0x00, 0xff, 0x10};

    CHECK(store->SetValue(kGameKey, "Detail", number).Ok());
    CHECK(store->SetValue(kGameKey, "Path", text).Ok());
    CHECK(store->SetValue(kGameKey, "", bytes).Ok());

    CHECK(store->GetValue(kGameKey, "detail").Value().dword == 0x12345678);
    CHECK(store->GetValue(kGameKey, "PATH").Value().text == "C:\\Games\\Test");
    CHECK(store->GetValue(kGameKey, "").Value().bytes == std::vector<std::uint8_t>{0x00, 0xff, 0x10});

    // Replacing a value changes its type.
    CHECK(store->SetValue(kGameKey, "DETAIL", text).Ok());
    CHECK(store->GetValue(kGameKey, "Detail").Value().type == RegistryType::kString);

    CHECK(store->GetValue(kGameKey, "Missing").Code() == Error::kNotFound);
    CHECK(store->GetValue("HKEY_LOCAL_MACHINE\\SOFTWARE\\Other", "Detail").Code() ==
          Error::kNotFound);
    CHECK(store->SetValue("HKEY_LOCAL_MACHINE\\SOFTWARE\\Other", "Detail", number).Code() ==
          Error::kNotFound);

    RegistryValue broken;
    broken.type = RegistryType::kString;
    broken.text = "two\nlines";
    CHECK(store->SetValue(kGameKey, "Broken", broken).Code() == Error::kInvalidArgument);
    CHECK(store->SetValue(kGameKey, "Tab\tName", number).Code() == Error::kInvalidArgument);
    CHECK(store->GetValue(kGameKey, "Broken").Code() == Error::kNotFound);
}

TEST_CASE("deleting a registry key removes everything below it, never a root key") {
    Folders folders;
    auto store = folders.open();
    REQUIRE(store->CreateKey(kGameKey).Ok());
    REQUIRE(store->CreateKey("HKEY_LOCAL_MACHINE\\SOFTWARE\\TestCompanyOther").Ok());
    CHECK(store->DeleteKey("HKEY_LOCAL_MACHINE\\SOFTWARE\\testcompany").Ok());
    CHECK_FALSE(store->KeyExists(kGameKey).Value());
    CHECK_FALSE(store->KeyExists("HKEY_LOCAL_MACHINE\\SOFTWARE\\TestCompany").Value());
    CHECK(store->KeyExists("HKEY_LOCAL_MACHINE\\SOFTWARE\\TestCompanyOther").Value());
    CHECK(store->KeyExists("HKEY_LOCAL_MACHINE\\SOFTWARE").Value());
    CHECK(store->DeleteKey(kGameKey).Code() == Error::kNotFound);
    CHECK(store->DeleteKey("HKEY_LOCAL_MACHINE").Code() == Error::kInvalidArgument);
}

TEST_CASE("registry.tsv holds every key and value and a fresh store on the folder reads them") {
    Folders folders;
    {
        auto store = folders.open();
        REQUIRE(store->CreateKey(kGameKey).Ok());
        REQUIRE(store->CreateKey("HKEY_CURRENT_USER\\Software\\Empty").Ok());
        RegistryValue number;
        number.dword = 0xA;
        RegistryValue text;
        text.type = RegistryType::kString;
        text.text = "TestCaptain";
        RegistryValue bytes;
        bytes.type = RegistryType::kBinary;
        bytes.bytes = {0xde, 0xad};
        REQUIRE(store->SetValue(kGameKey, "Detail", number).Ok());
        REQUIRE(store->SetValue(kGameKey, "Name", text).Ok());
        REQUIRE(store->SetValue(kGameKey, "Blob", bytes).Ok());
    }
    CHECK(readHostFile(folders.user / "registry.tsv") ==
          "# registry.tsv: key<TAB>name<TAB>type<TAB>value, one line per key (type key, name and "
          "value empty) and one per value (type dword: 8 hex digits; string: the bytes as stored; "
          "binary: two hex digits a byte); keys and names compare without ASCII case\n"
          "HKEY_CURRENT_USER\\Software\t\tkey\t\n"
          "HKEY_CURRENT_USER\\Software\\Empty\t\tkey\t\n"
          "HKEY_LOCAL_MACHINE\\SOFTWARE\t\tkey\t\n"
          "HKEY_LOCAL_MACHINE\\SOFTWARE\\TestCompany\t\tkey\t\n"
          "HKEY_LOCAL_MACHINE\\SOFTWARE\\TestCompany\\TestGame\t\tkey\t\n"
          "HKEY_LOCAL_MACHINE\\SOFTWARE\\TestCompany\\TestGame\tBlob\tbinary\tdead\n"
          "HKEY_LOCAL_MACHINE\\SOFTWARE\\TestCompany\\TestGame\tDetail\tdword\t0000000a\n"
          "HKEY_LOCAL_MACHINE\\SOFTWARE\\TestCompany\\TestGame\tName\tstring\tTestCaptain\n");

    auto fresh = folders.open();
    CHECK(fresh->KeyExists("HKEY_CURRENT_USER\\Software\\Empty").Value());
    CHECK(fresh->GetValue(kGameKey, "detail").Value().dword == 0xA);
    CHECK(fresh->GetValue(kGameKey, "name").Value().text == "TestCaptain");
    CHECK(fresh->GetValue(kGameKey, "blob").Value().bytes == std::vector<std::uint8_t>{0xde, 0xad});
}

TEST_CASE("a registry.tsv line that cannot be read is skipped, the others are kept") {
    Folders folders;
    writeHostFile(folders.user / "registry.tsv",
                  "# header\n"
                  "HKEY_CURRENT_USER\\Software\\Test\tGood\tdword\t00000001\n"
                  "HKEY_CURRENT_USER\\Software\\Test\tShort\tdword\t1\n"
                  "HKEY_CURRENT_USER\\Software\\Test\tOdd\tbinary\tabc\n"
                  "HKEY_CURRENT_USER\\Software\\Test\tWhat\tqword\t0\n"
                  "too few fields\n");
    auto store = folders.open();
    CHECK(store->GetValue("HKEY_CURRENT_USER\\Software\\Test", "Good").Value().dword == 1);
    CHECK(store->GetValue("HKEY_CURRENT_USER\\Software\\Test", "Short").Code() == Error::kNotFound);
    CHECK(store->GetValue("HKEY_CURRENT_USER\\Software\\Test", "Odd").Code() == Error::kNotFound);
    CHECK(store->GetValue("HKEY_CURRENT_USER\\Software\\Test", "What").Code() == Error::kNotFound);
}

TEST_CASE("a registry change that cannot be written changes nothing") {
    Folders folders;
    auto store = folders.open();
    REQUIRE(store->CreateKey(kGameKey).Ok());
    // registry.tsv cannot be replaced while a directory has its name.
    fs::remove(folders.user / "registry.tsv");
    fs::create_directories(folders.user / "registry.tsv");
    RegistryValue number;
    number.dword = 7;
    CHECK(store->SetValue(kGameKey, "Detail", number).Code() == Error::kIoError);
    CHECK(store->GetValue(kGameKey, "Detail").Code() == Error::kNotFound);
    CHECK(store->CreateKey("HKEY_CURRENT_USER\\Software\\New").Code() == Error::kIoError);
    CHECK_FALSE(store->KeyExists("HKEY_CURRENT_USER\\Software\\New").Value());
}

} // namespace
} // namespace sfcop::platform
