// The C functions the guest shims call: handles, GUID bytes, the game's record layout, the
// runtime's components and the event-handle lookup.
#include "hle/dinput/DinputBridge.h"
#include "hle/dinput/DInputDefs.h"
#include "platform/Input/InputMock.h"
#include "platform/Threads/ThreadsMock.h"

#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <vector>

namespace sfcop::hle::dinput {
namespace {

// GUID_SysKeyboard as the game's memory holds it.
constexpr std::uint8_t kSysKeyboardBytes[16] = {0x61, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11,
                                                0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
constexpr std::uint8_t kSysMouseBytes[16] = {0x60, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11,
                                             0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
constexpr std::uint8_t kKeyBytes[16] = {0x20, 0x82, 0x72, 0x55, 0x3C, 0xD3, 0xCF, 0x11,
                                        0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};

// Installs mocks as the runtime's components for one test, and removes them after.
struct Installed {
    platform::InputMock input;
    platform::ThreadsMock threads;

    Installed() {
        Platform platform;
        platform.input = &input;
        platform.threads = &threads;
        platform.waitableOfHandle = [](std::uint32_t handle) -> std::optional<platform::Waitable> {
            if (handle == 0x44) {
                return platform::Waitable{9};
            }
            return std::nullopt;
        };
        setPlatform(platform);
    }
    ~Installed() { setPlatform(Platform{}); }
};

std::uint32_t word(const std::uint8_t *at) {
    return static_cast<std::uint32_t>(at[0]) | static_cast<std::uint32_t>(at[1]) << 8 |
           static_cast<std::uint32_t>(at[2]) << 16 | static_cast<std::uint32_t>(at[3]) << 24;
}

std::vector<sfcop_dinput_object_format> keyboardObjects() {
    std::vector<sfcop_dinput_object_format> objects(256);
    for (std::uint32_t i = 0; i < 256; ++i) {
        objects[i].has_guid = 1;
        std::memcpy(objects[i].guid, kKeyBytes, 16);
        objects[i].ofs = i;
        objects[i].type = 0x8000000C | i << 8;
        objects[i].flags = 0;
    }
    return objects;
}

platform::InputEvent key(bool down, platform::Scancode usage, std::uint32_t timeMs) {
    platform::InputEvent event;
    event.type = down ? platform::InputEventType::kKeyDown : platform::InputEventType::kKeyUp;
    event.key = usage;
    event.timeMs = timeMs;
    return event;
}

} // namespace

TEST_CASE("the shims' C functions run the game's keyboard from creation to release") {
    Installed installed;
    std::uint32_t di = 0;
    REQUIRE(sfcop_dinput_create(0x0700, 0, &di) == kDiOk);
    CHECK(di != 0);

    std::uint32_t keyboard = 0;
    CHECK(sfcop_dinput_create_device(di, kSysMouseBytes, 0, &keyboard) == kDiErrDeviceNotReg);
    CHECK(keyboard == 0);
    REQUIRE(sfcop_dinput_create_device(di, kSysKeyboardBytes, 0, &keyboard) == kDiOk);
    CHECK(keyboard != 0);
    CHECK(keyboard != di);

    std::vector<sfcop_dinput_object_format> objects = keyboardObjects();
    sfcop_dinput_format format{24, 16, 2, 256, 256, objects.data()};
    REQUIRE(sfcop_dinput_device_set_data_format(keyboard, &format) == kDiOk);
    REQUIRE(sfcop_dinput_device_set_cooperative_level(keyboard, 1, 0x16) == kDiOk);
    sfcop_dinput_prop_header header{20, 16, 0, 0};
    REQUIRE(sfcop_dinput_device_set_property(keyboard, kPropBufferSize, &header, 16) == kDiOk);
    std::uint32_t size = 0;
    REQUIRE(sfcop_dinput_device_get_property(keyboard, kPropBufferSize, &header, &size) == kDiOk);
    CHECK(size == 16);
    REQUIRE(sfcop_dinput_device_acquire(keyboard) == kDiOk);

    installed.input.OnInputEvent(key(true, 0x04, 500));  // A, sequence 1
    installed.input.OnInputEvent(key(false, 0x04, 530)); // sequence 2
    // Two 20-byte DIDEVICEOBJECTDATA, filled with a pattern the call must overwrite.
    std::uint8_t records[48];
    std::memset(records, 0xEE, sizeof records);
    std::uint32_t count = kInfinite; // more than buffered: the call writes only what is there
    REQUIRE(sfcop_dinput_device_get_data(keyboard, 20, records, &count, 0) == kDiOk);
    REQUIRE(count == 2);
    CHECK(word(records + 0) == 0x1E);
    CHECK(word(records + 4) == 0x80);
    CHECK(word(records + 8) == 500);
    CHECK(word(records + 12) == 1);
    CHECK(word(records + 16) == 0); // uAppData
    CHECK(word(records + 20) == 0x1E);
    CHECK(word(records + 24) == 0x00);
    CHECK(word(records + 28) == 530);
    CHECK(word(records + 32) == 2);
    CHECK(word(records + 36) == 0);
    CHECK(records[40] == 0xEE); // nothing past the second record

    installed.input.keyboard[0x29] = true; // Escape held
    std::uint8_t state[256];
    REQUIRE(sfcop_dinput_device_get_state(keyboard, 256, state) == kDiOk);
    CHECK(state[0x01] == 0x80);
    CHECK(state[0x1E] == 0x00);

    CHECK(sfcop_dinput_device_unacquire(keyboard) == kDiOk);
    CHECK(sfcop_dinput_device_add_ref(keyboard) == 2);
    CHECK(sfcop_dinput_device_release(keyboard) == 1);
    CHECK(sfcop_dinput_device_release(keyboard) == 0);
    CHECK(sfcop_dinput_device_acquire(keyboard) == kDiErrInvalidParam); // the handle is gone
    CHECK(sfcop_dinput_release(di) == 0);
    CHECK(sfcop_dinput_get_device_status(di, kSysKeyboardBytes) == kDiErrInvalidParam);
}

TEST_CASE("the shims' enumerations hand out C structures with the names and GUIDs") {
    Installed installed;
    std::uint32_t di = 0;
    REQUIRE(sfcop_dinput_create(0x0700, 0, &di) == kDiOk);
    std::vector<std::string> names;
    auto device = [](const sfcop_dinput_device_instance *instance, void *context) {
        CHECK(std::memcmp(instance->instance, kSysKeyboardBytes, 16) == 0);
        CHECK(instance->dev_type == 0x0403);
        static_cast<std::vector<std::string> *>(context)->push_back(instance->product_name);
        return 1;
    };
    REQUIRE(sfcop_dinput_enum_devices(di, 0, device, &names, 0) == kDiOk);
    REQUIRE(names.size() == 1);
    CHECK(names[0] == "Keyboard");
    CHECK(sfcop_dinput_enum_devices(di, 0, nullptr, nullptr, 0) == kDiErrInvalidParam);

    std::uint32_t keyboard = 0;
    REQUIRE(sfcop_dinput_create_device(di, kSysKeyboardBytes, 0, &keyboard) == kDiOk);
    names.clear();
    auto object = [](const sfcop_dinput_object_instance *instance, void *context) {
        CHECK(std::memcmp(instance->guid_type, kKeyBytes, 16) == 0);
        auto *seen = static_cast<std::vector<std::string> *>(context);
        seen->push_back(instance->name);
        return seen->size() < 2 ? 1 : 0;
    };
    REQUIRE(sfcop_dinput_device_enum_objects(keyboard, object, &names, 0) == kDiOk);
    CHECK(names == std::vector<std::string>{"Esc", "1"});

    sfcop_dinput_object_instance info{};
    REQUIRE(sfcop_dinput_device_get_object_info(keyboard, 316, &info, 0x1C0C, 2) == kDiOk);
    CHECK(std::string(info.name) == "Enter");
    CHECK(info.ofs == 0x1C);
    sfcop_dinput_device_instance self{};
    REQUIRE(sfcop_dinput_device_get_device_info(keyboard, 580, &self) == kDiOk);
    CHECK(std::string(self.instance_name) == "Keyboard");
    sfcop_dinput_caps caps{};
    REQUIRE(sfcop_dinput_device_get_capabilities(keyboard, 44, &caps) == kDiOk);
    CHECK(caps.buttons == 119);
    CHECK(sfcop_dinput_device_initialize(keyboard, 0x0700, kSysKeyboardBytes) == kDiOk);
    CHECK(sfcop_dinput_device_initialize(keyboard, 0x0700, kSysMouseBytes) == kDiErrDeviceNotReg);
    CHECK(sfcop_dinput_device_run_control_panel(keyboard, 0) == kDiOk);
    CHECK(sfcop_dinput_get_device_status(di, kSysKeyboardBytes) == kDiOk);
    CHECK(sfcop_dinput_get_device_status(di, nullptr) == kDiErrInvalidParam);
    CHECK(sfcop_dinput_run_control_panel(di, 0) == kDiOk);
    CHECK(sfcop_dinput_initialize(di, 0x0700) == kDiOk);
    CHECK(sfcop_dinput_device_release(keyboard) == 0);
    CHECK(sfcop_dinput_release(di) == 0);
}

TEST_CASE("a game event handle becomes the runtime's Threads event, signalled by the pump") {
    Installed installed;
    std::uint32_t di = 0;
    std::uint32_t keyboard = 0;
    REQUIRE(sfcop_dinput_create(0x0700, 0, &di) == kDiOk);
    REQUIRE(sfcop_dinput_create_device(di, kSysKeyboardBytes, 0, &keyboard) == kDiOk);
    CHECK(sfcop_dinput_device_set_event_notification(keyboard, 0x45) == kDiErrInvalidParam);
    REQUIRE(sfcop_dinput_device_set_event_notification(keyboard, 0x44) == kDiOk);
    std::vector<sfcop_dinput_object_format> objects = keyboardObjects();
    sfcop_dinput_format format{24, 16, 2, 256, 256, objects.data()};
    REQUIRE(sfcop_dinput_device_set_data_format(keyboard, &format) == kDiOk);
    REQUIRE(sfcop_dinput_device_acquire(keyboard) == kDiOk);

    installed.input.OnInputEvent(key(true, 0x2C, 10));
    sfcop_dinput_pump();
    CHECK(installed.threads.log.Calls() == std::vector<std::string>{"SetEvent(9)"});

    sfcop_dinput_device_release(keyboard);
    sfcop_dinput_release(di);
}

TEST_CASE("without the runtime's components, objects work on an unfed input and never signal") {
    setPlatform(Platform{});
    std::uint32_t di = 0;
    std::uint32_t keyboard = 0;
    REQUIRE(sfcop_dinput_create(0x0700, 0, &di) == kDiOk);
    REQUIRE(sfcop_dinput_create_device(di, kSysKeyboardBytes, 0, &keyboard) == kDiOk);
    CHECK(sfcop_dinput_device_set_event_notification(keyboard, 0x44) == kDiOk);
    std::vector<sfcop_dinput_object_format> objects = keyboardObjects();
    sfcop_dinput_format format{24, 16, 2, 256, 256, objects.data()};
    REQUIRE(sfcop_dinput_device_set_data_format(keyboard, &format) == kDiOk);
    REQUIRE(sfcop_dinput_device_acquire(keyboard) == kDiOk);
    std::uint8_t state[256];
    std::memset(state, 0xEE, sizeof state);
    REQUIRE(sfcop_dinput_device_get_state(keyboard, 256, state) == kDiOk);
    for (std::uint8_t byte : state) {
        CHECK(byte == 0);
    }
    sfcop_dinput_pump();
    CHECK(sfcop_dinput_device_release(keyboard) == 0);
    CHECK(sfcop_dinput_release(di) == 0);
}

TEST_CASE("Release of an unknown handle frees nothing") {
    Installed installed;
    // Never made: the bridge says so, and the shim frees only on a count of 0.
    CHECK(sfcop_dinput_release(12345) == SFCOP_DINPUT_UNKNOWN_HANDLE);
    CHECK(sfcop_dinput_device_release(12345) == SFCOP_DINPUT_UNKNOWN_HANDLE);
    // Released to zero already: a second Release is unknown too, not another 0.
    std::uint32_t di = 0;
    std::uint32_t keyboard = 0;
    REQUIRE(sfcop_dinput_create(0x0700, 0, &di) == kDiOk);
    REQUIRE(sfcop_dinput_create_device(di, kSysKeyboardBytes, 0, &keyboard) == kDiOk);
    CHECK(sfcop_dinput_device_release(keyboard) == 0);
    CHECK(sfcop_dinput_device_release(keyboard) == SFCOP_DINPUT_UNKNOWN_HANDLE);
    CHECK(sfcop_dinput_release(di) == 0);
    CHECK(sfcop_dinput_release(di) == SFCOP_DINPUT_UNKNOWN_HANDLE);
}

TEST_CASE("the shims' C functions refuse null arguments and dead handles") {
    Installed installed;
    CHECK(sfcop_dinput_create(0x0700, 0, nullptr) == kDiErrInvalidParam);
    std::uint32_t handle = 7;
    CHECK(sfcop_dinput_create(0x0900, 0, &handle) == kDiErrOldDirectInputVersion);
    CHECK(handle == 0);
    CHECK(sfcop_dinput_add_ref(12345) == 0);
    CHECK(sfcop_dinput_create_device(12345, kSysKeyboardBytes, 0, &handle) == kDiErrInvalidParam);
    CHECK(sfcop_dinput_device_get_state(12345, 256, nullptr) == kDiErrInvalidParam);
    sfcop_dinput_prop_header header{20, 16, 0, 0};
    CHECK(sfcop_dinput_device_set_property(12345, kPropBufferSize, &header, 1) ==
          kDiErrInvalidParam);
    std::uint32_t di = 0;
    std::uint32_t keyboard = 0;
    REQUIRE(sfcop_dinput_create(0x0700, 0, &di) == kDiOk);
    REQUIRE(sfcop_dinput_create_device(di, kSysKeyboardBytes, 0, &keyboard) == kDiOk);
    CHECK(sfcop_dinput_device_set_property(keyboard, kPropBufferSize, nullptr, 1) ==
          kDiErrInvalidParam);
    CHECK(sfcop_dinput_device_set_data_format(keyboard, nullptr) == kDiErrInvalidParam);
    sfcop_dinput_format broken{24, 16, 2, 256, 3, nullptr};
    CHECK(sfcop_dinput_device_set_data_format(keyboard, &broken) == kDiErrInvalidParam);
    std::uint32_t count = 1;
    CHECK(sfcop_dinput_device_get_data(keyboard, 20, nullptr, &count, 0) == kDiErrNotAcquired);
    CHECK(sfcop_dinput_device_get_capabilities(keyboard, 44, nullptr) == kDiErrInvalidParam);
    CHECK(sfcop_dinput_device_get_object_info(keyboard, 316, nullptr, 0, 1) == kDiErrInvalidParam);
    CHECK(sfcop_dinput_device_get_device_info(keyboard, 580, nullptr) == kDiErrInvalidParam);
    sfcop_dinput_device_release(keyboard);
    sfcop_dinput_release(di);
}

} // namespace sfcop::hle::dinput
