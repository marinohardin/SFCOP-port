// IDirectInputDeviceA on the keyboard, called as the game calls it, through InputMock and
// ThreadsMock.
#include "hle/dinput/DinputTestUtil.h"
#include "hle/dinput/DirectInput.h"
#include "hle/dinput/KeyTable.h"
#include "platform/Input/InputMock.h"
#include "platform/Threads/ThreadsMock.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace sfcop::hle::dinput {
namespace {

using test::deviceHeader;
using test::keyboardFormat;
using test::keyEvent;
using test::mouseMoveEvent;

constexpr platform::Scancode kUsageA = 0x04;
constexpr platform::Scancode kUsageEscape = 0x29;
constexpr platform::Scancode kUsageSpace = 0x2C;
constexpr platform::Scancode kUsageLeftCtrl = 0xE0;
constexpr platform::Scancode kUsageRightCtrl = 0xE4;
constexpr platform::Scancode kUsageUp = 0x52;

// The game's DirectInput: one DirectInput object and its keyboard, released at the end.
struct Fixture {
    platform::InputMock input;
    platform::ThreadsMock threads;
    DirectInput *directInput = nullptr;
    KeyboardDevice *keyboard = nullptr;

    Fixture() {
        REQUIRE(DirectInput::Create(0x0700, false, input, threads, &directInput) == kDiOk);
        REQUIRE(directInput->CreateDevice(&kGuidSysKeyboard, &keyboard, false) == kDiOk);
    }
    ~Fixture() {
        keyboard->Release();
        directInput->Release();
    }

    // The game's set-up (surface.tsv notes): c_dfDIKeyboard, non-exclusive foreground without
    // the Windows key, then Acquire.
    void setUp(std::uint32_t bufferSize) {
        DataFormat format = keyboardFormat();
        REQUIRE(keyboard->SetDataFormat(&format) == kDiOk);
        REQUIRE(keyboard->SetCooperativeLevel(true, 0x16) == kDiOk);
        if (bufferSize != 0) {
            REQUIRE(keyboard->SetProperty(kPropBufferSize, deviceHeader(), bufferSize) == kDiOk);
        }
        REQUIRE(keyboard->Acquire() == kDiOk);
    }
};

std::vector<DeviceObjectData> read(KeyboardDevice &device, std::uint32_t count, HResult *result,
                                   std::uint32_t flags = 0) {
    std::vector<DeviceObjectData> records(count);
    std::uint32_t inOut = count;
    *result = device.GetDeviceData(20, records.data(), &inOut, flags);
    records.resize(inOut);
    return records;
}

void checkRecord(const DeviceObjectData &record, std::uint32_t ofs, std::uint32_t data,
                 std::uint32_t time, std::uint32_t sequence) {
    CHECK(record.ofs == ofs);
    CHECK(record.data == data);
    CHECK(record.timeStamp == time);
    CHECK(record.sequence == sequence);
}

} // namespace

TEST_CASE("the game's keyboard set-up succeeds, and acquiring twice has no effect") {
    Fixture f;
    f.setUp(0);
    CHECK(f.keyboard->Acquire() == kDiNoEffect);
    CHECK(f.keyboard->Unacquire() == kDiOk);
    CHECK(f.keyboard->Unacquire() == kDiNoEffect);
    CHECK(f.keyboard->Acquire() == kDiOk);
}

TEST_CASE("a keyboard cannot be acquired before it has a data format") {
    Fixture f;
    CHECK(f.keyboard->Acquire() == kDiErrInvalidParam);
}

TEST_CASE("the immediate state is 256 bytes by DIK code, 0x80 for each key down") {
    Fixture f;
    std::uint8_t state[256];
    CHECK(f.keyboard->GetDeviceState(256, state) == kDiErrNotAcquired);
    f.setUp(0);
    f.input.keyboard[kUsageA] = true;
    f.input.keyboard[kUsageRightCtrl] = true;
    f.input.keyboard[kUsageUp] = true;
    REQUIRE(f.keyboard->GetDeviceState(256, state) == kDiOk);
    for (int i = 0; i < 256; ++i) {
        std::uint8_t expected = (i == 0x1E || i == 0x9D || i == 0xC8) ? 0x80 : 0x00;
        CHECK_MESSAGE(state[i] == expected, "DIK ", i);
    }
    CHECK(f.keyboard->GetDeviceState(255, state) == kDiErrInvalidParam);
    CHECK(f.keyboard->GetDeviceState(256, nullptr) == kDiErrInvalidParam);
    f.input.failWith = platform::Error::kUnsupported;
    CHECK(f.keyboard->GetDeviceState(256, state) == kDiErrInputLost);
}

TEST_CASE("buffered data: a hand-written press and release sequence gives exactly these records") {
    Fixture f;
    f.setUp(8);
    // InputMock numbers every event it is given from 1, as Input numbers its queue.
    f.input.OnInputEvent(keyEvent(true, kUsageA, 1000));              // 1
    f.input.OnInputEvent(keyEvent(false, kUsageA, 1040));             // 2: in the same frame
    f.input.OnInputEvent(keyEvent(true, kUsageLeftCtrl, 1100));       // 3
    f.input.OnInputEvent(keyEvent(true, kUsageLeftCtrl, 1600, true)); // 4: auto-repeat, dropped
    f.input.OnInputEvent(mouseMoveEvent(1650));                       // 5: not the keyboard's
    f.input.OnInputEvent(keyEvent(true, kUsageEscape, 1700));         // 6
    f.input.OnInputEvent(keyEvent(false, kUsageLeftCtrl, 1750));      // 7
    f.input.OnInputEvent(keyEvent(false, kUsageEscape, 1760));        // 8

    HResult result = 0;
    std::vector<DeviceObjectData> first = read(*f.keyboard, 4, &result);
    CHECK(result == kDiOk);
    REQUIRE(first.size() == 4);
    checkRecord(first[0], 0x1E, 0x80, 1000, 1);
    checkRecord(first[1], 0x1E, 0x00, 1040, 2);
    checkRecord(first[2], 0x1D, 0x80, 1100, 3);
    checkRecord(first[3], 0x01, 0x80, 1700, 6);

    std::vector<DeviceObjectData> rest = read(*f.keyboard, 10, &result);
    CHECK(result == kDiOk);
    REQUIRE(rest.size() == 2);
    checkRecord(rest[0], 0x1D, 0x00, 1750, 7);
    checkRecord(rest[1], 0x01, 0x00, 1760, 8);

    CHECK(read(*f.keyboard, 10, &result).empty());
    CHECK(result == kDiOk);
}

TEST_CASE("a peek leaves the records, a null buffer counts or discards") {
    Fixture f;
    f.setUp(16);
    f.input.OnInputEvent(keyEvent(true, kUsageSpace, 10));
    f.input.OnInputEvent(keyEvent(false, kUsageSpace, 20));
    f.input.OnInputEvent(keyEvent(true, kUsageA, 30));
    f.input.keyboard[kUsageA] = true; // Input's state agrees with its events

    HResult result = 0;
    std::vector<DeviceObjectData> peeked = read(*f.keyboard, 1, &result, kGetDataPeek);
    REQUIRE(peeked.size() == 1);
    checkRecord(peeked[0], 0x39, 0x80, 10, 1);

    std::uint32_t count = kInfinite;
    CHECK(f.keyboard->GetDeviceData(16, nullptr, &count, kGetDataPeek) == kDiOk);
    CHECK(count == 3);

    count = 1;
    CHECK(f.keyboard->GetDeviceData(16, nullptr, &count, 0) == kDiOk); // discard the oldest
    CHECK(count == 1);
    std::vector<DeviceObjectData> left = read(*f.keyboard, 10, &result);
    REQUIRE(left.size() == 2);
    checkRecord(left[0], 0x39, 0x00, 20, 2);

    f.input.OnInputEvent(keyEvent(false, kUsageA, 40));
    f.input.keyboard[kUsageA] = false;
    count = kInfinite;
    CHECK(f.keyboard->GetDeviceData(20, nullptr, &count, 0) == kDiOk); // flush
    CHECK(count == 1);
}

TEST_CASE("a key held across a focus loss is released in the buffer") {
    Fixture f;
    f.setUp(8);
    f.input.OnInputEvent(keyEvent(true, kUsageA, 1000));          // sequence 1
    f.input.OnInputEvent(keyEvent(true, kUsageSpace, 1200));      // sequence 2
    f.input.keyboard[kUsageA] = true;
    f.input.keyboard[kUsageSpace] = true;
    HResult result = 0;
    std::vector<DeviceObjectData> pressed = read(*f.keyboard, 10, &result);
    REQUIRE(pressed.size() == 2);

    // The window loses the focus: Input clears its state and queues no key-up events.
    f.input.OnFocusChanged(false);
    std::uint8_t state[256];
    REQUIRE(f.keyboard->GetDeviceState(256, state) == kDiOk);
    CHECK(state[0x1E] == 0x00);

    std::vector<DeviceObjectData> released = read(*f.keyboard, 10, &result);
    CHECK(result == kDiOk);
    REQUIRE(released.size() == 2);
    // Both at the time of the last event seen, numbered after it, in DIK order of usage.
    checkRecord(released[0], 0x1E, 0x00, 1200, 3);
    checkRecord(released[1], 0x39, 0x00, 1200, 4);

    // Nothing more is owed, and a real release event that still arrives is not reported twice.
    CHECK(read(*f.keyboard, 10, &result).empty());
    f.input.OnInputEvent(keyEvent(false, kUsageA, 1300));
    CHECK(read(*f.keyboard, 10, &result).empty());
    f.input.OnInputEvent(keyEvent(true, kUsageA, 1400));
    f.input.keyboard[kUsageA] = true;
    std::vector<DeviceObjectData> again = read(*f.keyboard, 10, &result);
    REQUIRE(again.size() == 1);
    checkRecord(again[0], 0x1E, 0x80, 1400, 4);
}

TEST_CASE("a full buffer keeps the oldest records and reports the loss once") {
    Fixture f;
    f.setUp(2);
    f.input.OnInputEvent(keyEvent(true, kUsageA, 1));
    f.input.OnInputEvent(keyEvent(false, kUsageA, 2));
    f.input.OnInputEvent(keyEvent(true, kUsageSpace, 3));

    HResult result = 0;
    std::vector<DeviceObjectData> records = read(*f.keyboard, 2, &result, kGetDataPeek);
    CHECK(result == kDiBufferOverflow);
    records = read(*f.keyboard, 10, &result);
    CHECK(result == kDiBufferOverflow);
    REQUIRE(records.size() == 2);
    CHECK(records[0].timeStamp == 1);
    CHECK(records[1].timeStamp == 2);
    read(*f.keyboard, 10, &result);
    CHECK(result == kDiOk);

    // Events lost in Input's own queue are an overflow too.
    f.input.overflow = true;
    read(*f.keyboard, 10, &result);
    CHECK(result == kDiBufferOverflow);
}

TEST_CASE("buffered reads fail as DirectInput's do") {
    Fixture f;
    std::uint32_t count = 4;
    DeviceObjectData records[4];
    CHECK(f.keyboard->GetDeviceData(20, records, &count, 0) == kDiErrNotAcquired);
    f.setUp(0);
    CHECK(f.keyboard->GetDeviceData(20, records, &count, 0) == kDiErrNotBuffered);
    REQUIRE(f.keyboard->Unacquire() == kDiOk);
    REQUIRE(f.keyboard->SetProperty(kPropBufferSize, deviceHeader(), 4) == kDiOk);
    REQUIRE(f.keyboard->Acquire() == kDiOk);
    CHECK(f.keyboard->GetDeviceData(12, records, &count, 0) == kDiErrInvalidParam);
    CHECK(f.keyboard->GetDeviceData(20, records, nullptr, 0) == kDiErrInvalidParam);
    CHECK(f.keyboard->GetDeviceData(20, records, &count, 2) == kDiErrInvalidParam);
    CHECK(f.keyboard->GetDeviceData(16, records, &count, 0) == kDiOk);
}

TEST_CASE("keys pressed before Acquire or while unacquired are not buffered") {
    Fixture f;
    DataFormat format = keyboardFormat();
    REQUIRE(f.keyboard->SetDataFormat(&format) == kDiOk);
    REQUIRE(f.keyboard->SetProperty(kPropBufferSize, deviceHeader(), 8) == kDiOk);
    f.input.OnInputEvent(keyEvent(true, kUsageA, 1)); // 1
    REQUIRE(f.keyboard->Acquire() == kDiOk);
    f.input.OnInputEvent(keyEvent(false, kUsageA, 2)); // 2
    REQUIRE(f.keyboard->Unacquire() == kDiOk);
    f.input.OnInputEvent(keyEvent(true, kUsageSpace, 3)); // 3
    f.directInput->Pump();
    REQUIRE(f.keyboard->Acquire() == kDiOk);               // empties the buffer
    f.input.OnInputEvent(keyEvent(false, kUsageSpace, 4)); // 4

    HResult result = 0;
    std::vector<DeviceObjectData> records = read(*f.keyboard, 8, &result);
    REQUIRE(records.size() == 1);
    checkRecord(records[0], 0x39, 0x00, 4, 4);
}

TEST_CASE("the buffer size is a device property, fixed while acquired") {
    Fixture f;
    std::uint32_t value = 99;
    CHECK(f.keyboard->GetProperty(kPropBufferSize, deviceHeader(), &value) == kDiOk);
    CHECK(value == 0);
    CHECK(f.keyboard->SetProperty(kPropBufferSize, deviceHeader(), 32) == kDiOk);
    CHECK(f.keyboard->GetProperty(kPropBufferSize, deviceHeader(), &value) == kDiOk);
    CHECK(value == 32);

    CHECK(f.keyboard->SetProperty(kPropBufferSize, PropertyHeader{16, 16, 0, 0}, 1) ==
          kDiErrInvalidParam);
    CHECK(f.keyboard->SetProperty(kPropBufferSize, PropertyHeader{20, 16, 0x1E, 1}, 1) ==
          kDiErrInvalidParam);
    CHECK(f.keyboard->GetProperty(kPropBufferSize, deviceHeader(), nullptr) == kDiErrInvalidParam);
    CHECK(f.keyboard->SetProperty(2, deviceHeader(), 0) == kDiErrUnsupported); // DIPROP_AXISMODE
    CHECK(f.keyboard->GetProperty(4, deviceHeader(), &value) == kDiErrUnsupported); // RANGE

    f.setUp(0);
    CHECK(f.keyboard->SetProperty(kPropBufferSize, deviceHeader(), 8) == kDiErrAcquired);
    CHECK(f.keyboard->GetProperty(kPropBufferSize, deviceHeader(), &value) == kDiOk);
    CHECK(value == 32);
}

TEST_CASE("a data format places each key the format asks for at its offset") {
    Fixture f;
    DataFormat format;
    format.size = 24;
    format.objSize = 16;
    format.dataSize = 8;
    ObjectFormat space; // DIK_SPACE at 4
    space.guid = kGuidKey;
    space.ofs = 4;
    space.type = 0x0C | (0x39 << 8);
    ObjectFormat any; // any key, no GUID: the first in DIK order, Esc
    any.ofs = 0;
    any.type = 0x04 | 0x00FFFF00;
    ObjectFormat missing; // DIK 0x54 is no key; optional, so skipped
    missing.guid = kGuidKey;
    missing.ofs = 1;
    missing.type = 0x80000000 | 0x0C | (0x54 << 8);
    format.objects = {space, any, missing};
    REQUIRE(f.keyboard->SetDataFormat(&format) == kDiOk);
    REQUIRE(f.keyboard->SetProperty(kPropBufferSize, deviceHeader(), 4) == kDiOk);
    REQUIRE(f.keyboard->Acquire() == kDiOk);

    f.input.keyboard[kUsageSpace] = true;
    f.input.keyboard[kUsageEscape] = true;
    f.input.keyboard[kUsageA] = true; // not in this format
    std::uint8_t state[8];
    REQUIRE(f.keyboard->GetDeviceState(8, state) == kDiOk);
    const std::uint8_t expected[8] = {0x80, 0, 0, 0, 0x80, 0, 0, 0};
    for (int i = 0; i < 8; ++i) {
        CHECK(state[i] == expected[i]);
    }
    CHECK(f.keyboard->GetDeviceState(256, state) == kDiErrInvalidParam);

    f.input.OnInputEvent(keyEvent(true, kUsageA, 5));
    f.input.OnInputEvent(keyEvent(true, kUsageSpace, 6));
    HResult result = 0;
    std::vector<DeviceObjectData> records = read(*f.keyboard, 4, &result);
    REQUIRE(records.size() == 1);
    checkRecord(records[0], 4, 0x80, 6, 2);
}

TEST_CASE("a data format that is not a keyboard's is refused") {
    Fixture f;
    DataFormat format = keyboardFormat();
    format.size = 20;
    CHECK(f.keyboard->SetDataFormat(&format) == kDiErrInvalidParam);
    format = keyboardFormat();
    format.objSize = 12;
    CHECK(f.keyboard->SetDataFormat(&format) == kDiErrInvalidParam);
    format = keyboardFormat();
    format.dataSize = 255;
    CHECK(f.keyboard->SetDataFormat(&format) == kDiErrInvalidParam);
    format = keyboardFormat();
    format.objects[0].ofs = 256;
    CHECK(f.keyboard->SetDataFormat(&format) == kDiErrInvalidParam);
    format = keyboardFormat();
    format.objects[0x54].type &= ~0x80000000u; // a required key the keyboard does not have
    CHECK(f.keyboard->SetDataFormat(&format) == kDiErrInvalidParam);
    format = keyboardFormat();
    format.objects[1].guid =
        GUID{0xA36D02E0, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
    format.objects[1].type &= ~0x80000000u; // a required X axis
    CHECK(f.keyboard->SetDataFormat(&format) == kDiErrInvalidParam);
    CHECK(f.keyboard->SetDataFormat(nullptr) == kDiErrInvalidParam);

    f.setUp(0);
    format = keyboardFormat();
    CHECK(f.keyboard->SetDataFormat(&format) == kDiErrAcquired);
}

TEST_CASE("the notification event is signalled through Threads when an acquired keyboard "
          "changes") {
    Fixture f;
    REQUIRE(f.keyboard->SetEventNotification(platform::Waitable{7}) == kDiOk);
    f.setUp(4);
    f.threads.log.Clear();

    f.input.OnInputEvent(keyEvent(true, kUsageA, 1));
    f.directInput->Pump(); // the runtime's pump after the window's, without a read
    CHECK(f.threads.log.Calls() == std::vector<std::string>{"SetEvent(7)"});
    f.directInput->Pump(); // nothing new
    CHECK(f.threads.log.Calls().size() == 1);

    f.input.OnInputEvent(mouseMoveEvent(2));
    f.directInput->Pump();
    CHECK(f.threads.log.Calls().size() == 1); // not a keyboard change

    CHECK(f.keyboard->SetEventNotification(std::nullopt) == kDiErrAcquired);
    REQUIRE(f.keyboard->Unacquire() == kDiOk);
    f.input.OnInputEvent(keyEvent(false, kUsageA, 3));
    f.directInput->Pump();
    CHECK(f.threads.log.Calls().size() == 1); // unacquired

    REQUIRE(f.keyboard->SetEventNotification(std::nullopt) == kDiOk);
    REQUIRE(f.keyboard->Acquire() == kDiOk);
    f.input.OnInputEvent(keyEvent(true, kUsageA, 4));
    f.directInput->Pump();
    CHECK(f.threads.log.Calls().size() == 1); // no event any more
}

TEST_CASE("cooperative levels are checked as DirectInput checks a keyboard's") {
    Fixture f;
    CHECK(f.keyboard->SetCooperativeLevel(true, 0x16) == kDiOk);  // the game's
    CHECK(f.keyboard->SetCooperativeLevel(true, 0x05) == kDiOk);  // exclusive foreground
    CHECK(f.keyboard->SetCooperativeLevel(false, 0x0A) == kDiOk); // background needs no window
    CHECK(f.keyboard->SetCooperativeLevel(true, 0x09) == kDiErrUnsupported);
    CHECK(f.keyboard->SetCooperativeLevel(true, 0x03) == kDiErrInvalidParam);
    CHECK(f.keyboard->SetCooperativeLevel(true, 0x0E) == kDiErrInvalidParam);
    CHECK(f.keyboard->SetCooperativeLevel(true, 0x02) == kDiErrInvalidParam);
    CHECK(f.keyboard->SetCooperativeLevel(false, 0x06) == kDiErrInvalidParam);
    CHECK(f.keyboard->SetCooperativeLevel(true, 0x46) == kDiErrInvalidParam);
}

TEST_CASE("the capabilities are an attached enhanced keyboard with one button per key") {
    Fixture f;
    DeviceCaps caps;
    REQUIRE(f.keyboard->GetCapabilities(44, &caps) == kDiOk);
    CHECK(caps.flags == 1);
    CHECK(caps.devType == 0x0403);
    CHECK(caps.axes == 0);
    CHECK(caps.buttons == 119);
    CHECK(caps.povs == 0);
    CHECK(f.keyboard->GetCapabilities(24, &caps) == kDiOk);
    CHECK(f.keyboard->GetCapabilities(30, &caps) == kDiErrInvalidParam);
    CHECK(f.keyboard->GetCapabilities(44, nullptr) == kDiErrInvalidParam);
}

TEST_CASE("the keys of the format are enumerated in DIK order until the callback stops") {
    Fixture f;
    DataFormat format = keyboardFormat();
    REQUIRE(f.keyboard->SetDataFormat(&format) == kDiOk);
    std::vector<ObjectInstance> seen;
    auto all = [&seen](const ObjectInstance &object) {
        seen.push_back(object);
        return true;
    };
    REQUIRE(f.keyboard->EnumObjects(all, 0) == kDiOk);
    REQUIRE(seen.size() == 119);
    CHECK(sameGuid(seen[0].guidType, kGuidKey));
    CHECK(seen[0].ofs == 0x01);
    CHECK(seen[0].type == 0x0104);
    CHECK(seen[0].name == "Esc");
    CHECK(seen[29].ofs == 0x1E);
    CHECK(seen[29].name == "A");
    CHECK(seen[118].ofs == 0xDE);
    CHECK(seen[118].name == "Power");

    seen.clear();
    int calls = 0;
    auto three = [&calls](const ObjectInstance &) { return ++calls < 3; };
    REQUIRE(f.keyboard->EnumObjects(three, 0x0C) == kDiOk);
    CHECK(calls == 3);
    REQUIRE(f.keyboard->EnumObjects(all, 0x03) == kDiOk); // axes only
    CHECK(seen.empty());
    CHECK(f.keyboard->EnumObjects(nullptr, 0) == kDiErrInvalidParam);
}

TEST_CASE("object information is found by offset or by type and instance") {
    Fixture f;
    ObjectInstance info;
    CHECK(f.keyboard->GetObjectInfo(316, &info, 0x1E, kPropHowByOffset) == kDiErrObjectNotFound);
    DataFormat format = keyboardFormat();
    REQUIRE(f.keyboard->SetDataFormat(&format) == kDiOk);
    REQUIRE(f.keyboard->GetObjectInfo(316, &info, 0x1E, kPropHowByOffset) == kDiOk);
    CHECK(info.name == "A");
    CHECK(info.type == 0x1E04);
    REQUIRE(f.keyboard->GetObjectInfo(292, &info, 0xC80C, kPropHowById) == kDiOk);
    CHECK(info.name == "Up");
    CHECK(info.ofs == 0xC8);
    CHECK(f.keyboard->GetObjectInfo(316, &info, 0x54, kPropHowByOffset) == kDiErrObjectNotFound);
    CHECK(f.keyboard->GetObjectInfo(316, &info, 0x5403, kPropHowById) == kDiErrObjectNotFound);
    CHECK(f.keyboard->GetObjectInfo(316, &info, 0x1E, 3) == kDiErrInvalidParam);
    CHECK(f.keyboard->GetObjectInfo(100, &info, 0x1E, kPropHowByOffset) == kDiErrInvalidParam);
    CHECK(f.keyboard->GetObjectInfo(316, nullptr, 0x1E, kPropHowByOffset) == kDiErrInvalidParam);
}

TEST_CASE("the device describes itself as the system keyboard") {
    Fixture f;
    DeviceInstance info;
    REQUIRE(f.keyboard->GetDeviceInfo(580, &info) == kDiOk);
    CHECK(sameGuid(info.instance, kGuidSysKeyboard));
    CHECK(sameGuid(info.product, kGuidSysKeyboard));
    CHECK(info.devType == 0x0403);
    CHECK(info.instanceName == "Keyboard");
    CHECK(info.productName == "Keyboard");
    CHECK(f.keyboard->GetDeviceInfo(560, &info) == kDiOk);
    CHECK(f.keyboard->GetDeviceInfo(100, &info) == kDiErrInvalidParam);
    CHECK(f.keyboard->GetDeviceInfo(580, nullptr) == kDiErrInvalidParam);
}

TEST_CASE("the control panel and Initialize answer as for an initialised keyboard") {
    Fixture f;
    CHECK(f.keyboard->RunControlPanel(0) == kDiOk);
    CHECK(f.keyboard->Initialize(0x0700, &kGuidSysKeyboard) == kDiOk);
    CHECK(f.keyboard->Initialize(0x0800, &kGuidSysKeyboard) == kDiErrOldDirectInputVersion);
    CHECK(f.keyboard->Initialize(0x0700, &kGuidKey) == kDiErrDeviceNotReg);
    CHECK(f.keyboard->Initialize(0x0700, nullptr) == kDiErrInvalidParam);
}

TEST_CASE("the device's reference count follows AddRef and Release") {
    Fixture f;
    CHECK(f.keyboard->AddRef() == 2);
    CHECK(f.keyboard->Release() == 1);
}

TEST_CASE("the key table maps usage IDs to the DIK codes of the PC scan code set") {
    CHECK(dikFromUsage(0x04) == 0x1E); // A
    CHECK(dikFromUsage(0x1D) == 0x2C); // Z
    CHECK(dikFromUsage(0x27) == 0x0B); // 0
    CHECK(dikFromUsage(0x28) == 0x1C); // Return
    CHECK(dikFromUsage(0x2C) == 0x39); // Space
    CHECK(dikFromUsage(0x3A) == 0x3B); // F1
    CHECK(dikFromUsage(0x45) == 0x58); // F12
    CHECK(dikFromUsage(0x46) == 0xB7); // Print Screen: DIK_SYSRQ
    CHECK(dikFromUsage(0x48) == 0xC5); // Pause
    CHECK(dikFromUsage(0x52) == 0xC8); // Up
    CHECK(dikFromUsage(0x54) == 0xB5); // keypad /
    CHECK(dikFromUsage(0x58) == 0x9C); // keypad Enter
    CHECK(dikFromUsage(0x32) == 0x2B); // ISO # beside Enter: DIK_BACKSLASH
    CHECK(dikFromUsage(0x64) == 0x56); // ISO <>
    CHECK(dikFromUsage(0xE0) == 0x1D); // left Ctrl
    CHECK(dikFromUsage(0xE6) == 0xB8); // right Alt
    CHECK(dikFromUsage(0xE7) == 0xDC); // right GUI
    CHECK(dikFromUsage(0x00) == 0);
    CHECK(dikFromUsage(0x75) == 0); // Help: no DIK code
    CHECK(dikFromUsage(300) == 0);

    std::size_t count = 0;
    const KeyInfo *keys = keyTable(&count);
    REQUIRE(count == 119);
    for (std::size_t i = 1; i < count; ++i) {
        CHECK(keys[i - 1].dik < keys[i].dik);
    }
    CHECK(keyFromDik(0xC8)->usage == 0x52);
    CHECK(keyFromDik(0x54) == nullptr);
    CHECK(keyFromDik(256) == nullptr);
}

} // namespace sfcop::hle::dinput
