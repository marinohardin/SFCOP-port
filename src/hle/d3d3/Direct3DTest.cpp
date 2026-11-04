#include "hle/d3d3/D3d3TestRig.h"

namespace sfcop::hle::d3d3 {

using platform::PixelFormat;
using test::D3dRig;

TEST_CASE("EnumDevices offers one hardware device sized by the Gpu") {
    D3dRig rig;
    rig.gpu.capsResult = platform::GpuCaps{4096, {PixelFormat::kRgb565}, true};
    std::vector<DeviceInfo> devices;
    CHECK(rig.d3d->EnumDevices(devices) == kDdOk);
    REQUIRE(devices.size() == 1);
    const DeviceInfo &hal = devices[0];
    CHECK(hal.guid == kIidHalDevice);
    CHECK(hal.hardware.flags == 0x7FFu);
    CHECK(hal.hardware.colorModel == 2u);
    CHECK(hal.hardware.devCaps == 0x00082E51u);
    CHECK(hal.hardware.triCaps.zCmpCaps == 0xFFu);
    CHECK(hal.hardware.triCaps.srcBlendCaps == 0x1FFFu);
    CHECK(hal.hardware.triCaps.textureAddressCaps == 0x17u);
    CHECK(hal.hardware.renderBitDepth == 0x500u);
    CHECK(hal.hardware.zBufferBitDepth == 0x400u);
    CHECK(hal.hardware.maxTextureWidth == 4096u);
    CHECK(hal.hardware.textureOpCaps == 0xBAFFu);
    CHECK(hal.hardware.maxTextureBlendStages == 2);
    CHECK(hal.hardware.maxSimultaneousTextures == 2);
    CHECK(hal.software.devCaps == 0u);
}

TEST_CASE("FindDevice finds the hardware device and nothing else") {
    D3dRig rig;
    DeviceInfo found;
    DeviceSearch search;
    search.flags = kFdsHardware | kFdsColorModel;
    search.hardware = true;
    search.colorModel = kColorRgb;
    CHECK(rig.d3d->FindDevice(search, found) == kDdOk);
    CHECK(found.guid == kIidHalDevice);

    search = DeviceSearch{};
    search.flags = kFdsGuid;
    search.guid = kIidRgbDevice;
    CHECK(rig.d3d->FindDevice(search, found) == ddraw4::kDdErrNotFound);
    search.flags = kFdsHardware;
    search.hardware = false;
    CHECK(rig.d3d->FindDevice(search, found) == ddraw4::kDdErrNotFound);
}

TEST_CASE("CreateDevice draws into a 3D surface and refuses other targets") {
    D3dRig rig;
    Device *device = nullptr;
    CHECK(rig.d3d->CreateDevice(kIidHalDevice, rig.back, &device) == kDdOk);
    REQUIRE(device != nullptr);
    CHECK(&device->Target() == rig.back);
    CHECK(rig.back->RefCount() == 2); // the rig's and the device's
    device->Release();
    CHECK(rig.back->RefCount() == 1);

    ddraw4::Surface *plain = rig.Create(ddraw4::test::plainDesc(64, 64, PixelFormat::kRgb565));
    CHECK(rig.d3d->CreateDevice(kIidHalDevice, plain, &device) == ddraw4::kDdErrInvalidCaps);
    CHECK(device == nullptr);
    const Guid other = makeGuid(1, 2, 3, {4, 5, 6, 7, 8, 9, 10, 11});
    CHECK(rig.d3d->CreateDevice(other, rig.back, &device) == ddraw4::kDdErrInvalidParams);
    plain->Release();
}

TEST_CASE("EnumZBufferFormats offers 16-bit depth") {
    D3dRig rig;
    std::vector<ddraw4::PixelFormatDesc> formats;
    CHECK(rig.d3d->EnumZBufferFormats(kIidHalDevice, formats) == kDdOk);
    REQUIRE(formats.size() == 1);
    CHECK(formats[0].flags == ddraw4::kDdpfZBuffer);
    CHECK(formats[0].bitCount == 16);
    CHECK(formats[0].greenMask == 0xFFFFu); // dwZBitMask
}

TEST_CASE("the Direct3D object keeps its DirectDraw object alive") {
    D3dRig rig;
    Viewport *viewport = nullptr;
    REQUIRE(rig.d3d->CreateViewport(&viewport) == kDdOk);
    rig.d3d->Release(); // the viewport keeps it
    CHECK_FALSE(ddraw4::test::wasDestroyed(rig.d3d));
    viewport->Release();
    CHECK(ddraw4::test::wasDestroyed(rig.d3d));
}

} // namespace sfcop::hle::d3d3
