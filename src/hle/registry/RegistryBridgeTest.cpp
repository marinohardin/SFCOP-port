// The C bridge RegistryGuest.c calls, on a per-user folder of the test's own.

#include "hle/registry/RegistryBridge.h"

#include "hle/kernel/KernelTestFolders.h"
#include "hle/kernel/KernelTestStores.h"
#include "hle/registry/RegistryBridgeTesting.h"
#include "hle/registry/Registry.h"

#include <doctest/doctest.h>

#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>

namespace sfcop::hle::registry {
namespace {

using namespace win32;

TEST_CASE("a value set through the bridge survives a fresh attachment to the same folder") {
    kernel::test::TestFolders folders;
    sfcop_hle_registry_attach(folders.user.string().c_str());
    CHECK(sfcop_hle_registry_attached() == 1);
    std::uint32_t key = 0;
    std::uint32_t disposition = 0;
    REQUIRE(sfcop_registry_create_key(kLocalMachine, "SOFTWARE\\Quicksilver\\Debug", &key,
                                      &disposition) == kErrorSuccess);
    CHECK(disposition == kCreatedNewKey);
    const char name[] = "TestCaptain";
    CHECK(sfcop_registry_set_value(key, "Player", kRegSz,
                                   reinterpret_cast<const std::uint8_t *>(name),
                                   sizeof name) == kErrorSuccess);
    CHECK(sfcop_registry_close_key(key) == kErrorSuccess);
    CHECK(sfcop_registry_close_key(key) == kErrorInvalidHandle);

    sfcop_hle_registry_attach(folders.user.string().c_str());
    REQUIRE(sfcop_registry_open_key(kLocalMachine, "SOFTWARE\\Quicksilver\\Debug", &key) ==
            kErrorSuccess);
    std::uint32_t type = 0;
    std::uint8_t data[32] = {};
    std::uint32_t size = sizeof data;
    CHECK(sfcop_registry_query_value(key, "player", &type, data, &size) == kErrorSuccess);
    CHECK(type == kRegSz);
    CHECK(size == sizeof name);
    CHECK(std::memcmp(data, name, sizeof name) == 0);
    CHECK(sfcop_registry_delete_key(kLocalMachine, "SOFTWARE\\Quicksilver") == kErrorSuccess);
    CHECK(sfcop_registry_open_key(kLocalMachine, "SOFTWARE\\Quicksilver\\Debug", &key) ==
          kErrorFileNotFound);
    sfcop_hle_registry_detach();
}

TEST_CASE("the bridge's event log and its errors") {
    kernel::test::TestFolders folders;
    sfcop_hle_registry_attach(folders.user.string().c_str());
    std::uint32_t source = sfcop_registry_register_event_source(nullptr, "TestSource");
    CHECK(source != 0);
    CHECK(sfcop_registry_report_event(source) == 1);
    CHECK(sfcop_registry_deregister_event_source(source) == 1);
    std::uint32_t code = 0;
    CHECK(sfcop_registry_take_error(&code) == 0);
    CHECK(sfcop_registry_deregister_event_source(source) == 0);
    CHECK(sfcop_registry_take_error(&code) == 1);
    CHECK(code == kErrorInvalidHandle);
    CHECK_FALSE(std::filesystem::exists(folders.user / "registry.tsv"));
    sfcop_hle_registry_detach();
}

TEST_CASE("with nothing attached the registry calls fail") {
    sfcop_hle_registry_detach();
    CHECK(sfcop_hle_registry_attached() == 0);
    std::uint32_t key = 5;
    CHECK(sfcop_registry_open_key(kLocalMachine, "SOFTWARE", &key) == kErrorRegistryIoFailed);
    CHECK(sfcop_registry_close_key(0x1000) == kErrorInvalidHandle);
    CHECK(sfcop_registry_register_event_source(nullptr, "x") == 0);
}

void callEveryEntry(std::uint32_t expectedError) {
    std::uint32_t key = 5;
    std::uint32_t disposition = 0;
    std::uint8_t data[8] = {};
    std::uint32_t size = sizeof data;
    CHECK(sfcop_registry_open_key(kLocalMachine, "SOFTWARE\\Quicksilver", &key) == expectedError);
    CHECK(sfcop_registry_create_key(kLocalMachine, "SOFTWARE\\Quicksilver", &key, &disposition) ==
          expectedError);
    CHECK(sfcop_registry_delete_key(kLocalMachine, "SOFTWARE\\Quicksilver") == expectedError);
    CHECK(sfcop_registry_query_value(kLocalMachine, "x", nullptr, data, &size) == expectedError);
    CHECK(sfcop_registry_set_value(kLocalMachine, "x", kRegDword, data, 4) == expectedError);
    std::uint32_t code = 0;
    CHECK(sfcop_registry_take_error(&code) == 1); // the last of them left its error
    CHECK(code == expectedError);
}

TEST_CASE("an allocation failure in the Store is ERROR_NOT_ENOUGH_MEMORY, not an exception") {
    kernel::test::TestFolders folders;
    test::attachRegistryStore(std::make_unique<kernel::test::ThrowingStore>(
        platform::makeStore(std::string(), folders.user.string()), true));
    callEveryEntry(kErrorNotEnoughMemory);
    sfcop_hle_registry_detach();
}

TEST_CASE("any other exception in the Store is ERROR_GEN_FAILURE, not an exception") {
    kernel::test::TestFolders folders;
    test::attachRegistryStore(std::make_unique<kernel::test::ThrowingStore>(
        platform::makeStore(std::string(), folders.user.string()), false));
    callEveryEntry(kErrorGenFailure);
    sfcop_hle_registry_detach();
}

TEST_CASE("two threads' event-log errors stay with the thread that made them") {
    kernel::test::TestFolders folders;
    sfcop_hle_registry_attach(folders.user.string().c_str());
    std::mutex mutex;
    std::condition_variable changed;
    int step = 0;
    auto wait = [&](int mine) {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return step == mine; });
    };
    auto done = [&] {
        std::lock_guard<std::mutex> lock(mutex);
        step++;
        changed.notify_all();
    };
    int hadA = -1, hadB = -1;
    std::uint32_t codeA = 0, codeB = 0;
    std::thread a([&] {
        wait(0);
        sfcop_registry_deregister_event_source(0x7777); // fails: ERROR_INVALID_HANDLE
        done();
        wait(2);
        hadA = sfcop_registry_take_error(&codeA);
        done();
    });
    std::thread b([&] {
        wait(1);
        std::uint32_t source = sfcop_registry_register_event_source(nullptr, "TestSource");
        sfcop_registry_deregister_event_source(source); // succeeds: no error
        done();
        wait(3);
        hadB = sfcop_registry_take_error(&codeB);
        done();
    });
    a.join();
    b.join();
    CHECK(hadA == 1);
    CHECK(codeA == kErrorInvalidHandle);
    CHECK(hadB == 0);
    sfcop_hle_registry_detach();
}

} // namespace
} // namespace sfcop::hle::registry
