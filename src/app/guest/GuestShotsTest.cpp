// The screenshot encoder through the Gpu mock, with hand-written expected bytes.
#include "app/guest/GuestShots.h"
#include "platform/Gpu/GpuMock.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>

namespace sfcop::guest {

namespace {

// The 54-byte header of a 2 by 2, 24-bit BMP (70 bytes in all).
const std::vector<std::uint8_t> kHeader2x2 = {
    'B',  'M',  0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00,
    0x28, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x13, 0x0B, 0x00, 0x00,
    0x13, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

std::vector<std::uint8_t> bmpWith(const std::vector<std::uint8_t> &pixelData) {
    std::vector<std::uint8_t> bytes = kHeader2x2;
    bytes.insert(bytes.end(), pixelData.begin(), pixelData.end());
    return bytes;
}

} // namespace

TEST_CASE("a 2x2 Xrgb8888 image becomes a bottom-up 24-bit BMP") {
    // Top row: red, green. Bottom row: blue, white. Little-endian 0x00RRGGBB.
    const std::uint8_t pixels[16] = {0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF, 0x00, 0x00,
                                     0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x00};
    const std::vector<std::uint8_t> bmp =
        EncodeBmp(2, 2, platform::PixelFormat::kXrgb8888, pixels, 8);
    // Stored bottom row first, each pixel blue-green-red, rows padded to 8 bytes.
    const std::vector<std::uint8_t> expected = bmpWith({0xFF, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x00,
                                                        0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00});
    CHECK(bmp == expected);
}

TEST_CASE("Rgb565 channels expand to eight bits") {
    // Red 0xF800, then green 0x07E0; and blue 0x001F, then 0x0000.
    const std::uint8_t pixels[8] = {0x00, 0xF8, 0xE0, 0x07, 0x1F, 0x00, 0x00, 0x00};
    const std::vector<std::uint8_t> bmp = EncodeBmp(2, 2, platform::PixelFormat::kRgb565, pixels, 4);
    const std::vector<std::uint8_t> expected = bmpWith({0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                        0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00});
    CHECK(bmp == expected);
}

TEST_CASE("formats with no colour, and empty images, encode to nothing") {
    const std::uint8_t pixels[16] = {};
    CHECK(EncodeBmp(2, 2, platform::PixelFormat::kIndexed8, pixels, 2).empty());
    CHECK(EncodeBmp(2, 2, platform::PixelFormat::kDepth16, pixels, 4).empty());
    CHECK(EncodeBmp(0, 2, platform::PixelFormat::kXrgb8888, pixels, 0).empty());
}

TEST_CASE("capturing reads the whole texture through the Gpu") {
    platform::GpuMock gpu;
    gpu.readValue = 0x80; // every byte of the image
    const auto shot =
        CaptureBmp(gpu, platform::TextureId{1}, 2, 2, platform::PixelFormat::kXrgb8888);
    REQUIRE(shot.Ok());
    CHECK(gpu.log.Calls() == std::vector<std::string>{"ReadTexture(1, 0, 0, 0, 2, 2, 8)"});
    const std::vector<std::uint8_t> expected = bmpWith({0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x00,
                                                        0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x00});
    CHECK(shot.Value() == expected);
}

TEST_CASE("a failed read is reported with the Gpu's error") {
    platform::GpuMock gpu;
    gpu.failWith = platform::Error::kInvalidHandle;
    const auto shot =
        CaptureBmp(gpu, platform::TextureId{7}, 2, 2, platform::PixelFormat::kXrgb8888);
    CHECK_FALSE(shot.Ok());
    CHECK(shot.Code() == platform::Error::kInvalidHandle);
}

TEST_CASE("shot names sort by elapsed time and a shot is written to its folder") {
    namespace fs = std::filesystem;
    const fs::path directory = fs::temp_directory_path() / "sfcop_guest_shots_test" / "nested";
    fs::remove_all(directory.parent_path());
    CHECK(fs::path(ShotPath("d", 5000)).filename() == "shot_000005000.bmp");
    CHECK(ShotPath("d", 5000) < ShotPath("d", 10000));

    const std::string path = ShotPath(directory.string(), 1234);
    CHECK(WriteShot(path, {1, 2, 3}));
    std::ifstream file(path, std::ios::binary);
    const std::vector<char> back((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    CHECK(back == std::vector<char>{1, 2, 3});
    fs::remove_all(directory.parent_path());
}

TEST_CASE("a shot is due every interval, the first at the interval") {
    ShotSchedule schedule(5.0);
    CHECK_FALSE(schedule.Due(0.0));
    CHECK_FALSE(schedule.Due(4.9));
    CHECK(schedule.Due(5.0));
    CHECK_FALSE(schedule.Due(9.0));
    CHECK(schedule.Due(10.5));
    CHECK_FALSE(schedule.Due(12.0));
    ShotSchedule never(0.0);
    CHECK_FALSE(never.Due(100.0));
}

} // namespace sfcop::guest
