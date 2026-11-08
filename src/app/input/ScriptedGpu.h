// ScriptedGpu: the Gpu beside a ScriptedWindow. It forwards every call to the client's Gpu, except
// that Attach() given the ScriptedWindow attaches the inner Gpu to the window it wraps.
//
// The SDL backend finds the operating system's window of the Window object it is given by that
// object's address (GpuSdl.cpp, FindSdlWindow), so a Gpu attached to the wrapper itself would find
// none and the game's window would not open. Every other Window is passed on unchanged.
#pragma once

#include "app/input/ScriptedWindow.h"
#include "platform/Gpu/Gpu.h"

namespace sfcop::app::input {

class ScriptedGpu final : public platform::Gpu {
public:
    ScriptedGpu(platform::Gpu &inner, ScriptedWindow &window) : inner_(inner), window_(window) {}

    // The window Attach(given) attaches the inner Gpu to.
    platform::Window &AttachTarget(platform::Window &given) const;

    platform::Status Attach(platform::Window &window) override;
    platform::Status Detach() override;
    platform::Result<platform::GpuCaps> GetCaps() override;
    platform::Result<platform::TextureId> ConfigureBackBuffer(std::uint32_t width,
                                                              std::uint32_t height,
                                                              platform::PixelFormat format) override;
    platform::Result<platform::BufferId> CreateBuffer(platform::BufferKind kind,
                                                      std::uint32_t bytes) override;
    platform::Status WriteBuffer(platform::BufferId buffer, std::uint32_t offset, const void *data,
                                 std::uint32_t bytes) override;
    platform::Status DestroyBuffer(platform::BufferId buffer) override;
    platform::Result<platform::TextureId> CreateTexture(const platform::TextureDesc &desc) override;
    platform::Status WriteTexture(platform::TextureId texture, std::uint8_t level,
                                  platform::Rect region, const void *pixels,
                                  std::uint32_t rowPitch) override;
    platform::Status ReadTexture(platform::TextureId texture, std::uint8_t level,
                                 platform::Rect region, void *out, std::uint32_t rowPitch) override;
    platform::Status DestroyTexture(platform::TextureId texture) override;
    platform::Status SetColorKey(platform::TextureId texture,
                                 std::optional<std::uint32_t> key) override;
    platform::Result<platform::PaletteId> CreatePalette(const platform::PaletteEntry *entries) override;
    platform::Status WritePalette(platform::PaletteId palette, std::uint32_t first,
                                  std::uint32_t count, const platform::PaletteEntry *entries) override;
    platform::Status DestroyPalette(platform::PaletteId palette) override;
    platform::Status AttachPalette(platform::TextureId texture, platform::PaletteId palette) override;
    platform::Result<platform::PipelineId> CreatePipeline(const platform::PipelineDesc &desc) override;
    platform::Status DestroyPipeline(platform::PipelineId pipeline) override;
    platform::Status BeginScene(const platform::RenderTargets &targets) override;
    platform::Status SetViewport(const platform::Viewport &viewport) override;
    platform::Status Clear(const platform::ClearDesc &clear) override;
    platform::Status Draw(const platform::DrawCall &draw) override;
    platform::Status EndScene() override;
    platform::Status Blit(const platform::BlitDesc &blit) override;
    platform::Status Fill(platform::TextureId texture, platform::Rect area,
                          std::uint32_t value) override;
    platform::Status Present() override;
    platform::Status SetPresentFilter(platform::TextureFilter filter) override;

private:
    platform::Gpu &inner_;
    ScriptedWindow &window_;
};

} // namespace sfcop::app::input
