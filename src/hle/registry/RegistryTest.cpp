// The registry calls on Store: key handles, value sizes and errors as ADVAPI32 answers them.

#include "hle/registry/Registry.h"

#include "hle/kernel/KernelTestFolders.h"
#include "platform/Store/Store.h"
#include "platform/Store/StoreMock.h"

#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <vector>

namespace sfcop::hle::registry {
namespace {

using platform::Error;
using platform::RegistryType;
using platform::RegistryValue;
using platform::StoreMock;
using namespace win32;

const char kDebugKey[] = "SOFTWARE\\Quicksilver\\Debug";
const char kDebugName[] = "HKEY_LOCAL_MACHINE\\SOFTWARE\\Quicksilver\\Debug";

RegistryValue dwordValue(std::uint32_t number) {
    RegistryValue value;
    value.type = RegistryType::kDword;
    value.dword = number;
    return value;
}

RegistryValue stringValue(std::string text) {
    RegistryValue value;
    value.type = RegistryType::kString;
    value.text = std::move(text);
    return value;
}

TEST_CASE("RegOpenKeyExA opens an existing key by the game's path and refuses a missing one") {
    StoreMock store;
    Registry registry(store);
    std::uint32_t key = 0;
    CHECK(registry.OpenKey(kLocalMachine, kDebugKey, &key) == kErrorSuccess);
    CHECK(key != 0);
    CHECK(registry.KeyName(key) == std::string(kDebugName));
    CHECK(store.log.Calls() ==
          std::vector<std::string>{"KeyExists(\"HKEY_LOCAL_MACHINE\\SOFTWARE\\Quicksilver\\Debug\")"});

    std::uint32_t sub = 0;
    CHECK(registry.OpenKey(key, "Sub\\", &sub) == kErrorSuccess);
    CHECK(registry.KeyName(sub) == std::string(kDebugName) + "\\Sub");
    std::uint32_t same = 0;
    CHECK(registry.OpenKey(key, nullptr, &same) == kErrorSuccess);
    CHECK(same != key);
    CHECK(registry.KeyName(same) == std::string(kDebugName));

    store.keyExistsResult = false;
    std::uint32_t missing = 7;
    CHECK(registry.OpenKey(kCurrentUser, "Software\\None", &missing) == kErrorFileNotFound);
    CHECK(missing == 0);
    CHECK(registry.OpenKey(0x1234, "x", &missing) == kErrorInvalidHandle);
    CHECK(registry.OpenKey(kLocalMachine, "\\SOFTWARE", &missing) == kErrorBadPathname);
    CHECK(registry.OpenKey(kLocalMachine, "SOFTWARE", nullptr) == kErrorInvalidParameter);
    store.keyExistsResult = Error::kIoError;
    CHECK(registry.OpenKey(kLocalMachine, "SOFTWARE", &missing) == kErrorRegistryIoFailed);
}

TEST_CASE("RegCreateKeyExA creates a missing key and says whether it was new") {
    StoreMock store;
    Registry registry(store);
    std::uint32_t key = 0;
    std::uint32_t disposition = 0;
    store.keyExistsResult = false;
    CHECK(registry.CreateKey(kLocalMachine, kDebugKey, &key, &disposition) == kErrorSuccess);
    CHECK(disposition == kCreatedNewKey);
    CHECK(registry.KeyName(key) == std::string(kDebugName));
    CHECK(store.log.Calls() ==
          std::vector<std::string>{"KeyExists(\"HKEY_LOCAL_MACHINE\\SOFTWARE\\Quicksilver\\Debug\")",
                                   "CreateKey(\"HKEY_LOCAL_MACHINE\\SOFTWARE\\Quicksilver\\Debug\")"});

    store.keyExistsResult = true;
    store.log.Clear();
    CHECK(registry.CreateKey(kLocalMachine, kDebugKey, &key, &disposition) == kErrorSuccess);
    CHECK(disposition == kOpenedExistingKey);
    CHECK(store.log.Count("CreateKey") == 0);
    CHECK(registry.CreateKey(kLocalMachine, kDebugKey, &key, nullptr) == kErrorSuccess);

    store.keyExistsResult = false;
    store.createKeyResult = Error::kIoError;
    CHECK(registry.CreateKey(kLocalMachine, kDebugKey, &key, &disposition) == kErrorRegistryIoFailed);
    CHECK(key == 0);
}

TEST_CASE("RegCloseKey closes a handle once; a predefined key closes without effect") {
    StoreMock store;
    Registry registry(store);
    std::uint32_t key = 0;
    REQUIRE(registry.OpenKey(kLocalMachine, kDebugKey, &key) == kErrorSuccess);
    CHECK(registry.CloseKey(key) == kErrorSuccess);
    CHECK(registry.CloseKey(key) == kErrorInvalidHandle);
    CHECK_FALSE(registry.KeyName(key).has_value());
    CHECK(registry.CloseKey(kLocalMachine) == kErrorSuccess);
    CHECK(registry.KeyName(kLocalMachine) == std::string("HKEY_LOCAL_MACHINE"));
}

TEST_CASE("RegDeleteKeyA deletes the named subkey; a predefined key is refused") {
    StoreMock store;
    Registry registry(store);
    CHECK(registry.DeleteKey(kLocalMachine, kDebugKey) == kErrorSuccess);
    CHECK(store.log.Calls() ==
          std::vector<std::string>{"DeleteKey(\"HKEY_LOCAL_MACHINE\\SOFTWARE\\Quicksilver\\Debug\")"});
    CHECK(registry.DeleteKey(kLocalMachine, "") == kErrorAccessDenied);
    CHECK(registry.DeleteKey(kLocalMachine, nullptr) == kErrorInvalidParameter);
    store.deleteKeyResult = Error::kNotFound;
    CHECK(registry.DeleteKey(kLocalMachine, "SOFTWARE\\None") == kErrorFileNotFound);
}

TEST_CASE("RegQueryValueExA answers a DWORD's type, size and bytes") {
    StoreMock store;
    store.registry[std::string(kDebugName) + "|Level"] = dwordValue(0x01020304);
    Registry registry(store);
    std::uint32_t key = 0;
    REQUIRE(registry.OpenKey(kLocalMachine, kDebugKey, &key) == kErrorSuccess);

    std::uint32_t type = 99;
    std::uint8_t data[8];
    std::memset(data, 0xEE, sizeof data);
    std::uint32_t size = sizeof data;
    CHECK(registry.QueryValue(key, "Level", &type, data, &size) == kErrorSuccess);
    CHECK(type == kRegDword);
    CHECK(size == 4);
    const std::uint8_t expected[8] = {0x04, 0x03, 0x02, 0x01, 0xEE, 0xEE, 0xEE, 0xEE};
    CHECK(std::memcmp(data, expected, sizeof data) == 0);

    type = 99;
    CHECK(registry.QueryValue(key, "Level", &type, nullptr, nullptr) == kErrorSuccess);
    CHECK(type == kRegDword);
    size = 0;
    CHECK(registry.QueryValue(key, "Level", nullptr, nullptr, &size) == kErrorSuccess);
    CHECK(size == 4);
}

TEST_CASE("RegQueryValueExA counts a string's NUL and answers ERROR_MORE_DATA when short") {
    StoreMock store;
    store.registry[std::string(kDebugName) + "|Name"] = stringValue("TestCaptain");
    Registry registry(store);
    std::uint32_t key = 0;
    REQUIRE(registry.OpenKey(kLocalMachine, kDebugKey, &key) == kErrorSuccess);

    char text[16];
    std::memset(text, '#', sizeof text);
    std::uint32_t size = 5;
    std::uint32_t type = 0;
    CHECK(registry.QueryValue(key, "Name", &type, reinterpret_cast<std::uint8_t *>(text), &size) ==
          kErrorMoreData);
    CHECK(size == 12);
    CHECK(type == kRegSz);
    CHECK(text[0] == '#');

    size = 12;
    CHECK(registry.QueryValue(key, "Name", &type, reinterpret_cast<std::uint8_t *>(text), &size) ==
          kErrorSuccess);
    CHECK(size == 12);
    CHECK(std::memcmp(text, "TestCaptain\0#", 13) == 0);

    CHECK(registry.QueryValue(key, "Name", &type, reinterpret_cast<std::uint8_t *>(text), nullptr) ==
          kErrorInvalidParameter);
    CHECK(registry.QueryValue(key, "Missing", &type, nullptr, &size) == kErrorFileNotFound);
    CHECK(registry.QueryValue(0x1234, "Name", &type, nullptr, &size) == kErrorInvalidHandle);
}

TEST_CASE("RegSetValueExA stores a DWORD, a string up to its NUL and binary bytes") {
    StoreMock store;
    Registry registry(store);
    std::uint32_t key = 0;
    REQUIRE(registry.OpenKey(kLocalMachine, kDebugKey, &key) == kErrorSuccess);
    store.log.Clear();

    const std::uint8_t number[4] = {0x2A, 0x00, 0x00, 0x80};
    CHECK(registry.SetValue(key, "Level", kRegDword, number, 4) == kErrorSuccess);
    CHECK(store.lastSetValue.type == RegistryType::kDword);
    CHECK(store.lastSetValue.dword == 0x8000002Au);

    const char text[] = "TestCaptain\0junk";
    CHECK(registry.SetValue(key, "Name", kRegSz, reinterpret_cast<const std::uint8_t *>(text),
                            sizeof text) == kErrorSuccess);
    CHECK(store.lastSetValue.type == RegistryType::kString);
    CHECK(store.lastSetValue.text == "TestCaptain");

    const std::uint8_t blob[3] = {0x00, 0xFF, 0x10};
    CHECK(registry.SetValue(key, "Blob", kRegBinary, blob, 3) == kErrorSuccess);
    CHECK(store.lastSetValue.type == RegistryType::kBinary);
    CHECK(store.lastSetValue.bytes == std::vector<std::uint8_t>{0x00, 0xFF, 0x10});

    CHECK(store.log.Calls() ==
          std::vector<std::string>{
              "SetValue(\"HKEY_LOCAL_MACHINE\\SOFTWARE\\Quicksilver\\Debug\", \"Level\", 0)",
              "SetValue(\"HKEY_LOCAL_MACHINE\\SOFTWARE\\Quicksilver\\Debug\", \"Name\", 1)",
              "SetValue(\"HKEY_LOCAL_MACHINE\\SOFTWARE\\Quicksilver\\Debug\", \"Blob\", 2)"});

    CHECK(registry.SetValue(key, "Level", kRegDword, number, 2) == kErrorInvalidParameter);
    CHECK(registry.SetValue(key, "Multi", 7, blob, 3) == kErrorInvalidParameter);
    CHECK(registry.SetValue(key, "Name", kRegSz, nullptr, 4) == kErrorInvalidParameter);
    CHECK(registry.SetValue(0x1234, "Level", kRegDword, number, 4) == kErrorInvalidHandle);
    CHECK(store.log.Count("SetValue") == 3);
}

TEST_CASE("the event log opens, accepts a report and closes, and records nothing") {
    StoreMock store;
    Registry registry(store);
    std::uint32_t source = registry.RegisterEventSource(nullptr, "TestSource");
    CHECK(source != 0);
    CHECK(registry.ReportEvent(source));
    CHECK(registry.DeregisterEventSource(source));
    CHECK_FALSE(registry.TakeLastError().has_value());
    CHECK_FALSE(registry.ReportEvent(source));
    CHECK(registry.TakeLastError() == kErrorInvalidHandle);
    CHECK_FALSE(registry.DeregisterEventSource(source));
    CHECK(registry.TakeLastError() == kErrorInvalidHandle);
    CHECK(store.log.Calls().empty());
}

TEST_CASE("a value set through RegSetValueExA reads back by the game's path and from a fresh Store") {
    kernel::test::TestFolders folders;
    {
        std::unique_ptr<platform::Store> store =
            platform::makeStore(folders.install.string(), folders.user.string());
        Registry registry(*store);
        std::uint32_t key = 0;
        std::uint32_t disposition = 0;
        REQUIRE(registry.CreateKey(kLocalMachine, kDebugKey, &key, &disposition) == kErrorSuccess);
        CHECK(disposition == kCreatedNewKey);
        const std::uint8_t number[4] = {0x05, 0x00, 0x00, 0x00};
        CHECK(registry.SetValue(key, "Level", kRegDword, number, 4) == kErrorSuccess);
        CHECK(registry.CloseKey(key) == kErrorSuccess);

        std::uint32_t reopened = 0;
        REQUIRE(registry.OpenKey(kLocalMachine, "software\\QUICKSILVER\\debug", &reopened) ==
                kErrorSuccess);
        std::uint32_t type = 0;
        std::uint8_t data[4] = {};
        std::uint32_t size = sizeof data;
        CHECK(registry.QueryValue(reopened, "LEVEL", &type, data, &size) == kErrorSuccess);
        CHECK(type == kRegDword);
        CHECK(size == 4);
        CHECK(data[0] == 0x05);
    }
    std::unique_ptr<platform::Store> fresh =
        platform::makeStore(folders.install.string(), folders.user.string());
    Registry registry(*fresh);
    std::uint32_t key = 0;
    std::uint32_t disposition = 0;
    REQUIRE(registry.CreateKey(kLocalMachine, kDebugKey, &key, &disposition) == kErrorSuccess);
    CHECK(disposition == kOpenedExistingKey);
    std::uint32_t type = 0;
    std::uint8_t data[4] = {};
    std::uint32_t size = sizeof data;
    CHECK(registry.QueryValue(key, "Level", &type, data, &size) == kErrorSuccess);
    CHECK(type == kRegDword);
    CHECK(size == 4);
    const std::uint8_t expected[4] = {0x05, 0x00, 0x00, 0x00};
    CHECK(std::memcmp(data, expected, 4) == 0);
    CHECK(std::filesystem::exists(folders.user / "registry.tsv"));
    CHECK(std::filesystem::is_empty(folders.install));
}

} // namespace
} // namespace sfcop::hle::registry
