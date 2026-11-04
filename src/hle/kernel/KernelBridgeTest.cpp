// The C bridge KernelGuest.c calls: the game's strings in, the game's answers and structures out.

#include "hle/kernel/KernelBridge.h"
#include "hle/kernel/KernelBridgeTesting.h"
#include "hle/kernel/KernelTestFolders.h"
#include "hle/kernel/KernelTestStores.h"

#include <doctest/doctest.h>

#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sfcop::hle::kernel {
namespace {

using test::GameFolders;
using namespace win32;

// The bridge's one kernel on this test's folders, detached afterwards.
struct Bridged : GameFolders {
    Bridged() { sfcop_hle_kernel_attach(install.string().c_str(), user.string().c_str()); }
    ~Bridged() { sfcop_hle_kernel_detach(); }
};

std::uint32_t takeError() {
    std::uint32_t code = 0;
    return sfcop_kernel_take_error(&code) != 0 ? code : 0;
}

std::uint32_t u32At(const std::uint8_t *data, std::size_t at) {
    return static_cast<std::uint32_t>(data[at]) | static_cast<std::uint32_t>(data[at + 1]) << 8 |
           static_cast<std::uint32_t>(data[at + 2]) << 16 |
           static_cast<std::uint32_t>(data[at + 3]) << 24;
}

TEST_CASE("an ANSI file written through the bridge reads back and lands in the per-user folder") {
    Bridged game;
    game.InstallFile("ClientSettings.gf", "install copy");
    const char content[] = "[Display]\r\nWidth=1024\r\n";
    std::uint32_t file = sfcop_kernel_create_file("C:\\ClientSettings.gf", 0,
                                                  kGenericRead | kGenericWrite, 0, kCreateAlways, 0);
    REQUIRE(file != 0);
    CHECK(sfcop_kernel_is_file(file) == 1);
    std::uint32_t written = 0;
    CHECK(sfcop_kernel_write_file(file, content, sizeof content - 1, &written) == 1);
    CHECK(written == sizeof content - 1);
    CHECK(sfcop_kernel_set_file_pointer(file, 0, nullptr, kFileBegin) == 0);
    char back[64] = {};
    std::uint32_t read = 0;
    CHECK(sfcop_kernel_read_file(file, back, sizeof back, &read) == 1);
    CHECK(std::string(back, read) == std::string(content, sizeof content - 1));
    std::uint32_t high = 7;
    CHECK(sfcop_kernel_get_file_size(file, &high) == sizeof content - 1);
    CHECK(high == 0);
    CHECK(sfcop_kernel_close_file(file) == 1);
    CHECK(sfcop_kernel_is_file(file) == 0);
    CHECK(game.UserContent("settings/ClientSettings.gf") == std::string(content, sizeof content - 1));
    CHECK(game.InstallContent("ClientSettings.gf") == "install copy");
}

TEST_CASE("names cross the bridge as Latin-1 or UTF-16") {
    Bridged game;
    game.InstallFile("Assets/caf\xc3\xa9.txt", "x");
    CHECK(sfcop_kernel_get_file_attributes("C:\\Assets\\CAF\xc9.TXT", 0) == kAttributeArchive);
    const std::uint16_t wide[] = {'C', ':', '\\', 'A', 's', 's', 'e', 't', 's', '\\',
                                  'c', 'a', 'f', 0xE9, '.', 't', 'x', 't', 0};
    CHECK(sfcop_kernel_get_file_attributes(wide, 1) == kAttributeArchive);
    CHECK(sfcop_kernel_get_file_attributes("C:\\Assets\\none.txt", 0) == kInvalidFileAttributes);
    CHECK(takeError() == kErrorFileNotFound);
}

TEST_CASE("WIN32_FIND_DATAA and W are written field by field") {
    Bridged game;
    game.InstallFile("Assets/caf\xc3\xa9.txt", "12345");

    std::vector<std::uint8_t> a(SFCOP_FIND_DATA_A_SIZE, 0xEE);
    std::uint32_t find = sfcop_kernel_find_first("C:\\Assets\\*.txt", 0, a.data());
    REQUIRE(find != 0);
    CHECK(u32At(a.data(), 0) == kAttributeArchive);
    CHECK(u32At(a.data(), 20) != 0);              // the write time, low half
    CHECK(u32At(a.data(), 4) == u32At(a.data(), 20)); // creation = write
    CHECK(u32At(a.data(), 28) == 0);              // size, high
    CHECK(u32At(a.data(), 32) == 5);              // size, low
    CHECK(u32At(a.data(), 36) == 0);
    CHECK(std::memcmp(a.data() + 44, "caf\xe9.txt\0", 9) == 0);
    CHECK(a[304] == 0);
    CHECK(a[319] == 0);
    CHECK(sfcop_kernel_find_next(find, a.data(), 0) == 0);
    CHECK(takeError() == kErrorNoMoreFiles);
    CHECK(sfcop_kernel_is_find(find) == 1);
    CHECK(sfcop_kernel_find_close(find) == 1);
    CHECK(sfcop_kernel_is_find(find) == 0);

    std::vector<std::uint8_t> w(SFCOP_FIND_DATA_W_SIZE, 0xEE);
    const std::uint16_t pattern[] = {'A', 's', 's', 'e', 't', 's', '\\', '*', 0};
    find = sfcop_kernel_find_first(pattern, 1, w.data());
    REQUIRE(find != 0);
    CHECK(u32At(w.data(), 0) == kAttributeDirectory);
    CHECK(w[44] == '.');
    CHECK(w[45] == 0);
    CHECK(w[46] == 0);
    REQUIRE(sfcop_kernel_find_next(find, w.data(), 1) == 1); // ".."
    REQUIRE(sfcop_kernel_find_next(find, w.data(), 1) == 1);
    CHECK(u32At(w.data(), 32) == 5);
    const std::uint8_t name[] = {'c', 0, 'a', 0, 'f', 0, 0xE9, 0, '.', 0, 't', 0, 'x', 0, 't', 0, 0, 0};
    CHECK(std::memcmp(w.data() + 44, name, sizeof name) == 0);
    CHECK(w[564] == 0);
    CHECK(w[591] == 0);
    CHECK(sfcop_kernel_find_close(find) == 1);
}

TEST_CASE("the directory answers count characters and ask for the NUL when short") {
    Bridged game;
    char buffer[16];
    CHECK(sfcop_kernel_current_directory(buffer, sizeof buffer, 0) == 3);
    CHECK(std::string(buffer) == "C:\\");
    CHECK(sfcop_kernel_current_directory(buffer, 3, 0) == 4);
    std::uint16_t wide[16];
    CHECK(sfcop_kernel_windows_directory(wide, 16, 1, 0) == 10);
    CHECK(wide[0] == 'C');
    CHECK(wide[10] == 0);
    CHECK(sfcop_kernel_windows_directory(buffer, sizeof buffer, 0, 1) == 18);
    CHECK(sfcop_kernel_module_file_name(buffer, sizeof buffer) == 16);
    CHECK(std::string(buffer) == "C:\\StarFleetOP.");
    CHECK(takeError() == 122);
    char module[32];
    CHECK(sfcop_kernel_module_file_name(module, sizeof module) == 18);
    CHECK(std::string(module) == "C:\\StarFleetOP.exe");
}

TEST_CASE("GetFullPathNameA gives the offset of the file part") {
    Bridged game;
    game.InstallFile("Assets/a.txt", "");
    CHECK(sfcop_kernel_set_current_directory("Assets", 0) == 1);
    char buffer[64];
    std::uint32_t part = 0;
    CHECK(sfcop_kernel_full_path_name("a.txt", buffer, sizeof buffer, &part) == 15);
    CHECK(std::string(buffer) == "C:\\Assets\\a.txt");
    CHECK(part == 10);
    CHECK(sfcop_kernel_full_path_name("a.txt", buffer, 5, &part) == 16);
    CHECK(part == UINT32_MAX);
}

TEST_CASE("GetVolumeInformation fills what is asked and refuses a short name buffer") {
    Bridged game;
    char volume[32];
    char system[32];
    std::uint32_t serial = 0;
    std::uint32_t length = 0;
    std::uint32_t flags = 0;
    CHECK(sfcop_kernel_volume_information("C:\\", 0, volume, sizeof volume, &serial, &length,
                                          &flags, system, sizeof system) == 1);
    CHECK(std::string(volume) == "SFCOP");
    CHECK(std::string(system) == "NTFS");
    CHECK(serial == 0x5FC00255u);
    CHECK(length == 255);
    CHECK(sfcop_kernel_volume_information("C:\\", 0, nullptr, 0, nullptr, nullptr, nullptr,
                                          nullptr, 0) == 1);
    CHECK(sfcop_kernel_volume_information("C:\\", 0, volume, 3, nullptr, nullptr, nullptr,
                                          nullptr, 0) == 0);
    CHECK(takeError() == 24);
}

TEST_CASE("the .ini rows reach the per-user copy") {
    Bridged game;
    game.InstallFile("sfc.ini", "[Game]\r\nSpeed=3\r\n");
    CHECK(sfcop_kernel_get_profile_int("Game", "Speed", 1, "C:\\sfc.ini") == 3);
    CHECK(sfcop_kernel_write_profile_string("Game", "Speed", "5", "C:\\sfc.ini") == 1);
    char buffer[8];
    CHECK(sfcop_kernel_get_profile_string("Game", "Speed", "", buffer, sizeof buffer,
                                          "C:\\SFC.INI") == 1);
    CHECK(std::string(buffer) == "5");
    CHECK(game.InstallContent("sfc.ini") == "[Game]\r\nSpeed=3\r\n");
    CHECK(game.UserHas("settings/sfc.ini"));
}

TEST_CASE("with nothing attached every call fails as a missing path") {
    sfcop_hle_kernel_detach();
    CHECK(sfcop_hle_kernel_attached() == 0);
    CHECK(sfcop_kernel_create_file("C:\\a.gf", 0, kGenericRead, 0, kOpenExisting, 0) == 0);
    CHECK(takeError() == kErrorPathNotFound);
    CHECK(sfcop_kernel_get_profile_int("a", "b", -4, "C:\\x.ini") == 0xFFFFFFFCu);
    CHECK(sfcop_kernel_is_file(1) == 0);
}

TEST_CASE("the default per-user folder is DataDir.h's") {
    char folder[1024];
    std::size_t length = sfcop_hle_default_user_dir(folder, sizeof folder);
    REQUIRE(length > 5);
    CHECK(std::string(folder).substr(length - 5) == "SFCOP");
    CHECK(sfcop_hle_default_user_dir(folder, 3) == 0);
}

// The kernel attached to a Store that throws; detached afterwards.
struct Throwing : GameFolders {
    explicit Throwing(bool allocation) {
        test::attachKernelStore(std::make_unique<test::ThrowingStore>(
            platform::makeStore(install.string(), user.string()), allocation));
    }
    ~Throwing() { sfcop_hle_kernel_detach(); }
};

// Every bridge entry that reaches the Store, called with the Store throwing: none lets the
// exception out, each answers its failure value and the error the row documents.
void callEveryStoreEntry(std::uint32_t expectedError) {
    std::uint8_t found[SFCOP_FIND_DATA_A_SIZE];
    char text[64];

    CHECK(sfcop_kernel_create_file("C:\\x.txt", 0, kGenericRead, 0, kOpenExisting, 0) == 0);
    CHECK(takeError() == expectedError);
    CHECK(sfcop_kernel_create_file("C:\\x.txt", 0, kGenericWrite, 0, kCreateAlways, 0) == 0);
    CHECK(takeError() == expectedError);
    CHECK(sfcop_kernel_delete_file("C:\\x.txt", 0) == 0);
    CHECK(takeError() == expectedError);
    CHECK(sfcop_kernel_move_file("C:\\x.txt", "C:\\y.txt") == 0);
    CHECK(takeError() == expectedError);
    CHECK(sfcop_kernel_copy_file("C:\\x.txt", "C:\\y.txt", 0) == 0);
    CHECK(takeError() == expectedError);
    CHECK(sfcop_kernel_create_directory("C:\\d", 0) == 0);
    CHECK(takeError() == expectedError);
    CHECK(sfcop_kernel_remove_directory("C:\\d", 0) == 0);
    CHECK(takeError() == expectedError);
    CHECK(sfcop_kernel_get_file_attributes("C:\\x.txt", 0) == kInvalidFileAttributes);
    CHECK(takeError() == expectedError);
    CHECK(sfcop_kernel_set_file_attributes("C:\\x.txt", 0, 0) == 0);
    CHECK(takeError() == expectedError);
    CHECK(sfcop_kernel_find_first("C:\\*", 0, found) == 0);
    CHECK(takeError() == expectedError);
    CHECK(sfcop_kernel_set_current_directory("C:\\Assets", 0) == 0);
    CHECK(takeError() == expectedError);
    CHECK(sfcop_kernel_write_profile_string("S", "K", "V", "x.ini") == 0);
    CHECK(takeError() == expectedError);
    CHECK(sfcop_kernel_get_profile_string("S", "K", "d", text, sizeof text, "C:\\x.ini") == 0);
    CHECK(takeError() == expectedError);
}

TEST_CASE("an allocation failure in the Store becomes ERROR_NOT_ENOUGH_MEMORY, not an exception") {
    Throwing game(true);
    callEveryStoreEntry(kErrorNotEnoughMemory);
}

TEST_CASE("any other exception in the Store becomes ERROR_GEN_FAILURE, not an exception") {
    Throwing game(false);
    callEveryStoreEntry(kErrorGenFailure);
}

TEST_CASE("an exception on a call after the first still reaches the game as a failure") {
    GameFolders folders;
    folders.InstallFile("a.txt", "x");
    auto store = std::make_unique<test::ThrowingStore>(
        platform::makeStore(folders.install.string(), folders.user.string()), false);
    store->passThrough = 1; // the first Store call (the lookup's Stat) goes through
    test::attachKernelStore(std::move(store));
    CHECK(sfcop_kernel_create_file("C:\\a.txt", 0, kGenericRead, 0, kOpenExisting, 0) == 0);
    CHECK(takeError() == kErrorGenFailure);
    sfcop_hle_kernel_detach();
}

// Two threads take turns, step by step: `turn` is whose step comes next.
struct Lockstep {
    std::mutex mutex;
    std::condition_variable changed;
    int step = 0;
    void Wait(int mine) {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return step == mine; });
    }
    void Done() {
        std::lock_guard<std::mutex> lock(mutex);
        step++;
        changed.notify_all();
    }
};

TEST_CASE("two threads in lockstep each read their own last error") {
    Bridged game;
    Lockstep turns;
    std::uint32_t errorA1 = 99, errorB1 = 99, errorA2 = 99, errorB2 = 99;
    int hadB2 = -1;

    std::thread a([&] {
        turns.Wait(0);
        sfcop_kernel_get_file_attributes("C:\\none.txt", 0); // ERROR_FILE_NOT_FOUND
        turns.Done();
        turns.Wait(2);
        errorA1 = takeError();
        sfcop_kernel_get_file_attributes("C:\\none.txt", 0); // fails again
        turns.Done();
        turns.Wait(5);
        errorA2 = takeError(); // B's calls in between must not have taken or replaced it
        turns.Done();
    });
    std::thread b([&] {
        turns.Wait(1);
        sfcop_kernel_get_file_attributes("C:\\nofolder\\none.txt", 0); // ERROR_PATH_NOT_FOUND
        turns.Done();
        turns.Wait(3);
        errorB1 = takeError();
        turns.Done();
        turns.Wait(4);
        char buffer[64];
        sfcop_kernel_current_directory(buffer, sizeof buffer, 0); // succeeds: no error
        std::uint32_t code = 0;
        hadB2 = sfcop_kernel_take_error(&code);
        errorB2 = code;
        turns.Done();
    });
    a.join();
    b.join();
    CHECK(errorA1 == kErrorFileNotFound);
    CHECK(errorB1 == kErrorPathNotFound);
    CHECK(errorA2 == kErrorFileNotFound);
    CHECK(hadB2 == 0);
    CHECK(errorB2 == 0);
}

} // namespace
} // namespace sfcop::hle::kernel
