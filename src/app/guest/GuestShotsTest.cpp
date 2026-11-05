// The screenshot encoder through the Gpu mock, with hand-written expected bytes.
#include "app/guest/GuestShots.h"
#include "hle/ddraw4/Ddraw4TestRig.h"
#include "platform/Gpu/GpuMock.h"
#include "platform/Window/WindowMock.h"

#include <doctest/doctest.h>

#include <cstring>
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

namespace {

// A Gpu that keeps the pixels of every texture's level 0, enough for DirectDraw to present a 2D
// frame and for a shot to read it back; everything else is kUnsupported.
class FrameGpu final : public platform::Gpu {
public:
    int presents = 0;
    int uploads = 0;

    platform::Status Attach(platform::Window &) override { return {}; }
    platform::Status Detach() override { return {}; }
    platform::Result<platform::GpuCaps> GetCaps() override { return platform::GpuCaps{}; }
    platform::Result<platform::TextureId> ConfigureBackBuffer(std::uint32_t width,
                                                              std::uint32_t height,
                                                              platform::PixelFormat format) override {
        return make(width, height, format); // black, as the contract says
    }
    platform::Result<platform::BufferId> CreateBuffer(platform::BufferKind, std::uint32_t) override {
        return platform::Error::kUnsupported;
    }
    platform::Status WriteBuffer(platform::BufferId, std::uint32_t, const void *,
                                 std::uint32_t) override {
        return platform::Error::kUnsupported;
    }
    platform::Status DestroyBuffer(platform::BufferId) override {
        return platform::Error::kUnsupported;
    }
    platform::Result<platform::TextureId> CreateTexture(const platform::TextureDesc &desc) override {
        return make(desc.width, desc.height, desc.format);
    }
    platform::Status WriteTexture(platform::TextureId texture, std::uint8_t, platform::Rect region,
                                  const void *pixels, std::uint32_t rowPitch) override {
        Texture &t = textures_.at(texture.value - 1);
        const std::uint32_t bytes = platform::pixelBytes(t.format);
        for (std::uint32_t y = 0; y < region.height; ++y) {
            std::memcpy(&t.pixels[((region.y + y) * t.width + region.x) * bytes],
                        static_cast<const std::uint8_t *>(pixels) + y * rowPitch,
                        region.width * bytes);
        }
        ++uploads;
        return {};
    }
    platform::Status ReadTexture(platform::TextureId texture, std::uint8_t, platform::Rect region,
                                 void *out, std::uint32_t rowPitch) override {
        const Texture &t = textures_.at(texture.value - 1);
        const std::uint32_t bytes = platform::pixelBytes(t.format);
        for (std::uint32_t y = 0; y < region.height; ++y) {
            std::memcpy(static_cast<std::uint8_t *>(out) + y * rowPitch,
                        &t.pixels[((region.y + y) * t.width + region.x) * bytes],
                        region.width * bytes);
        }
        return {};
    }
    platform::Status DestroyTexture(platform::TextureId) override { return {}; }
    platform::Status SetColorKey(platform::TextureId, std::optional<std::uint32_t>) override {
        return {};
    }
    platform::Result<platform::PaletteId> CreatePalette(const platform::PaletteEntry *) override {
        return platform::Error::kUnsupported;
    }
    platform::Status WritePalette(platform::PaletteId, std::uint32_t, std::uint32_t,
                                  const platform::PaletteEntry *) override {
        return platform::Error::kUnsupported;
    }
    platform::Status DestroyPalette(platform::PaletteId) override {
        return platform::Error::kUnsupported;
    }
    platform::Status AttachPalette(platform::TextureId, platform::PaletteId) override { return {}; }
    platform::Result<platform::PipelineId> CreatePipeline(const platform::PipelineDesc &) override {
        return platform::Error::kUnsupported;
    }
    platform::Status DestroyPipeline(platform::PipelineId) override {
        return platform::Error::kUnsupported;
    }
    platform::Status BeginScene(const platform::RenderTargets &) override {
        return platform::Error::kUnsupported;
    }
    platform::Status SetViewport(const platform::Viewport &) override {
        return platform::Error::kUnsupported;
    }
    platform::Status Clear(const platform::ClearDesc &) override {
        return platform::Error::kUnsupported;
    }
    platform::Status Draw(const platform::DrawCall &) override {
        return platform::Error::kUnsupported;
    }
    platform::Status EndScene() override { return platform::Error::kUnsupported; }
    platform::Status Blit(const platform::BlitDesc &) override {
        return platform::Error::kUnsupported;
    }
    platform::Status Fill(platform::TextureId, platform::Rect, std::uint32_t) override {
        return platform::Error::kUnsupported;
    }
    platform::Status Present() override {
        ++presents;
        return {};
    }

private:
    struct Texture {
        std::uint32_t width;
        platform::PixelFormat format;
        std::vector<std::uint8_t> pixels;
    };
    platform::TextureId make(std::uint32_t width, std::uint32_t height,
                             platform::PixelFormat format) {
        textures_.push_back(Texture{
            width, format,
            std::vector<std::uint8_t>(std::size_t{width} * height * platform::pixelBytes(format))});
        return platform::TextureId{static_cast<std::uint32_t>(textures_.size())};
    }
    std::vector<Texture> textures_;
};

} // namespace

TEST_CASE("a shot is the frame the game presented through Flip, not what it draws next") {
    FrameGpu gpu;
    platform::WindowMock window;
    hle::ddraw4::HeapPixelMemory memory;
    auto *draw = new hle::ddraw4::DirectDraw(gpu, window, memory);
    REQUIRE(draw->SetDisplayMode(2, 2, 16, 0, 0) == hle::ddraw4::kDdOk);
    hle::ddraw4::Surface *front = nullptr;
    REQUIRE(draw->CreateSurface(hle::ddraw4::test::flipChainDesc(), &front) ==
            hle::ddraw4::kDdOk);
    hle::ddraw4::Surface *back = nullptr;
    REQUIRE(front->GetAttachedSurface(
                hle::ddraw4::SurfaceCaps{hle::ddraw4::kDdscapsBackBuffer, 0, 0, 0}, &back) ==
            hle::ddraw4::kDdOk);

    // The frame: red, green on top; blue, white below (RGB565). Then the next one is drawn but
    // not flipped yet.
    hle::ddraw4::test::writePixels16(*back, {0xF800, 0x07E0, 0x001F, 0xFFFF});
    REQUIRE(front->Flip(nullptr, 0) == hle::ddraw4::kDdOk);
    hle::ddraw4::test::writePixels16(*back, {0, 0, 0, 0});
    CHECK(gpu.presents == 1);
    CHECK(gpu.uploads == 1);

    // What takeShot() does with the DirectDraw object that owns the display.
    REQUIRE(draw->OwnsDisplay());
    const auto texture = draw->DisplayTexture();
    REQUIRE(texture.Ok());
    const auto shot = CaptureBmp(gpu, texture.Value(), draw->Mode().width, draw->Mode().height,
                                 draw->PrimaryFormat());
    REQUIRE(shot.Ok());
    // Bottom row first: blue, white; then red, green; each blue-green-red, padded to 8 bytes.
    CHECK(shot.Value() == bmpWith({0xFF, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x00,
                                   0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00}));
    back->Release();
    front->Release();
    draw->Release();
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
