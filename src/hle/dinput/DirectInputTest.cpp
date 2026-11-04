// IDirectInputA, called as the game calls it, through InputMock and ThreadsMock.
#include "hle/dinput/DirectInput.h"
#include "hle/dinput/DinputTestUtil.h"
#include "platform/Input/InputMock.h"
#include "platform/Threads/ThreadsMock.h"

#include <doctest/doctest.h>

#include <vector>

namespace sfcop::hle::dinput {
namespace {

// {6F1D2B60-D5A0-11CF-BFC7-444553540000}: the system mouse, which is not served.
constexpr GUID kGuidSysMouse = {
    0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

} // namespace

TEST_CASE("DirectInputCreateA accepts the game's version 0x0700 and DirectX 3 to 7") {
    platform::InputMock input;
    platform::ThreadsMock threads;
    DirectInput *object = nullptr;
    REQUIRE(DirectInput::Create(0x0700, false, input, threads, &object) == kDiOk);
    REQUIRE(object != nullptr);
    CHECK(object->Release() == 0);
    REQUIRE(DirectInput::Create(0x0300, false, input, threads, &object) == kDiOk);
    CHECK(object->Release() == 0);
    REQUIRE(DirectInput::Create(0x0500, false, input, threads, &object) == kDiOk);
    CHECK(object->Release() == 0);
}

TEST_CASE("DirectInputCreateA refuses other versions, aggregation and a null out-pointer") {
    platform::InputMock input;
    platform::ThreadsMock threads;
    DirectInput *object = reinterpret_cast<DirectInput *>(0x1); // overwritten with null
    CHECK(DirectInput::Create(0x0800, false, input, threads, &object) ==
          kDiErrOldDirectInputVersion);
    CHECK(object == nullptr);
    CHECK(DirectInput::Create(0x0200, false, input, threads, &object) ==
          kDiErrBetaDirectInputVersion);
    CHECK(DirectInput::Create(0, false, input, threads, &object) == kDiErrBetaDirectInputVersion);
    CHECK(DirectInput::Create(0x0700, true, input, threads, &object) == kDiErrNoAggregation);
    CHECK(object == nullptr);
    CHECK(DirectInput::Create(0x0700, false, input, threads, nullptr) == kDiErrInvalidParam);
}

TEST_CASE("CreateDevice serves the system keyboard and nothing else") {
    platform::InputMock input;
    platform::ThreadsMock threads;
    DirectInput *object = nullptr;
    REQUIRE(DirectInput::Create(0x0700, false, input, threads, &object) == kDiOk);

    KeyboardDevice *keyboard = nullptr;
    REQUIRE(object->CreateDevice(&kGuidSysKeyboard, &keyboard, false) == kDiOk);
    REQUIRE(keyboard != nullptr);

    KeyboardDevice *other = keyboard;
    CHECK(object->CreateDevice(&kGuidSysMouse, &other, false) == kDiErrDeviceNotReg);
    CHECK(other == nullptr);
    CHECK(object->CreateDevice(nullptr, &other, false) == kDiErrInvalidParam);
    CHECK(object->CreateDevice(&kGuidSysKeyboard, nullptr, false) == kDiErrInvalidParam);
    CHECK(object->CreateDevice(&kGuidSysKeyboard, &other, true) == kDiErrNoAggregation);
    CHECK(other == nullptr);

    CHECK(keyboard->Release() == 0);
    CHECK(object->Release() == 0);
}

TEST_CASE("EnumDevices lists the keyboard for all devices and for keyboards only") {
    platform::InputMock input;
    platform::ThreadsMock threads;
    DirectInput *object = nullptr;
    REQUIRE(DirectInput::Create(0x0700, false, input, threads, &object) == kDiOk);
    std::vector<DeviceInstance> seen;
    auto collect = [&seen](const DeviceInstance &device) {
        seen.push_back(device);
        return true;
    };

    REQUIRE(object->EnumDevices(0, collect, 0) == kDiOk);
    REQUIRE(seen.size() == 1);
    CHECK(sameGuid(seen[0].instance, kGuidSysKeyboard));
    CHECK(seen[0].devType == 0x0403);
    CHECK(seen[0].productName == "Keyboard");

    REQUIRE(object->EnumDevices(3, collect, 1) == kDiOk); // keyboards, attached only
    CHECK(seen.size() == 2);
    REQUIRE(object->EnumDevices(0x0103, collect, 0) == kDiOk); // a subtype in the second byte
    CHECK(seen.size() == 3);
    REQUIRE(object->EnumDevices(2, collect, 0) == kDiOk);     // mice
    REQUIRE(object->EnumDevices(4, collect, 0) == kDiOk);     // joysticks
    REQUIRE(object->EnumDevices(0, collect, 0x100) == kDiOk); // force feedback
    CHECK(seen.size() == 3);
    CHECK(object->EnumDevices(0, nullptr, 0) == kDiErrInvalidParam);
    object->Release();
}

TEST_CASE("GetDeviceStatus, RunControlPanel and Initialize answer for the keyboard") {
    platform::InputMock input;
    platform::ThreadsMock threads;
    DirectInput *object = nullptr;
    REQUIRE(DirectInput::Create(0x0700, false, input, threads, &object) == kDiOk);
    CHECK(object->GetDeviceStatus(&kGuidSysKeyboard) == kDiOk);
    CHECK(object->GetDeviceStatus(&kGuidSysMouse) == kDiErrDeviceNotReg);
    CHECK(object->GetDeviceStatus(nullptr) == kDiErrInvalidParam);
    CHECK(object->RunControlPanel(0) == kDiOk);
    CHECK(object->Initialize(0x0700) == kDiOk);
    CHECK(object->Initialize(0x0800) == kDiErrOldDirectInputVersion);
    CHECK(object->AddRef() == 2);
    CHECK(object->Release() == 1);
    CHECK(object->Release() == 0);
}

TEST_CASE("a keyboard outlives the DirectInput object that made it, and two keyboards both see "
          "each key") {
    platform::InputMock input;
    platform::ThreadsMock threads;
    DirectInput *object = nullptr;
    REQUIRE(DirectInput::Create(0x0700, false, input, threads, &object) == kDiOk);
    KeyboardDevice *first = nullptr;
    KeyboardDevice *second = nullptr;
    REQUIRE(object->CreateDevice(&kGuidSysKeyboard, &first, false) == kDiOk);
    REQUIRE(object->CreateDevice(&kGuidSysKeyboard, &second, false) == kDiOk);
    CHECK(object->Release() == 0); // the game releases the devices first, but need not

    DataFormat format = test::keyboardFormat();
    for (KeyboardDevice *device : {first, second}) {
        REQUIRE(device->SetDataFormat(&format) == kDiOk);
        REQUIRE(device->SetProperty(kPropBufferSize, test::deviceHeader(), 4) == kDiOk);
        REQUIRE(device->Acquire() == kDiOk);
    }
    input.OnInputEvent(test::keyEvent(true, 0x29, 100));
    for (KeyboardDevice *device : {first, second}) {
        DeviceObjectData record;
        std::uint32_t count = 1;
        REQUIRE(device->GetDeviceData(16, &record, &count, 0) == kDiOk);
        REQUIRE(count == 1);
        CHECK(record.ofs == 0x01);
        CHECK(record.data == 0x80);
        CHECK(record.sequence == 1);
    }
    CHECK(first->Release() == 0);
    CHECK(second->Release() == 0);
}

} // namespace sfcop::hle::dinput
