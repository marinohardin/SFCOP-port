// The stub backend: compiles on every platform and supports nothing.
#include "platform/Gpu/Gpu.h"

namespace sfcop::platform {
namespace {

class GpuStub final : public Gpu {
public:
    Status Attach(Window &) override { return Error::kUnsupported; }
    Status Detach() override { return Error::kUnsupported; }
    Result<GpuCaps> GetCaps() override { return Error::kUnsupported; }
    Result<TextureId> ConfigureBackBuffer(std::uint32_t, std::uint32_t, PixelFormat) override {
        return Error::kUnsupported;
    }
    Result<BufferId> CreateBuffer(BufferKind, std::uint32_t) override {
        return Error::kUnsupported;
    }
    Status WriteBuffer(BufferId, std::uint32_t, const void *, std::uint32_t) override {
        return Error::kUnsupported;
    }
    Status DestroyBuffer(BufferId) override { return Error::kUnsupported; }
    Result<TextureId> CreateTexture(const TextureDesc &) override { return Error::kUnsupported; }
    Status WriteTexture(TextureId, std::uint8_t, Rect, const void *, std::uint32_t) override {
        return Error::kUnsupported;
    }
    Status ReadTexture(TextureId, std::uint8_t, Rect, void *, std::uint32_t) override {
        return Error::kUnsupported;
    }
    Status DestroyTexture(TextureId) override { return Error::kUnsupported; }
    Status SetColorKey(TextureId, std::optional<std::uint32_t>) override {
        return Error::kUnsupported;
    }
    Result<PaletteId> CreatePalette(const PaletteEntry *) override { return Error::kUnsupported; }
    Status WritePalette(PaletteId, std::uint32_t, std::uint32_t, const PaletteEntry *) override {
        return Error::kUnsupported;
    }
    Status DestroyPalette(PaletteId) override { return Error::kUnsupported; }
    Status AttachPalette(TextureId, PaletteId) override { return Error::kUnsupported; }
    Result<PipelineId> CreatePipeline(const PipelineDesc &) override {
        return Error::kUnsupported;
    }
    Status DestroyPipeline(PipelineId) override { return Error::kUnsupported; }
    Status BeginScene(const RenderTargets &) override { return Error::kUnsupported; }
    Status SetViewport(const Viewport &) override { return Error::kUnsupported; }
    Status Clear(const ClearDesc &) override { return Error::kUnsupported; }
    Status Draw(const DrawCall &) override { return Error::kUnsupported; }
    Status EndScene() override { return Error::kUnsupported; }
    Status Blit(const BlitDesc &) override { return Error::kUnsupported; }
    Status Fill(TextureId, Rect, std::uint32_t) override { return Error::kUnsupported; }
    Status Present() override { return Error::kUnsupported; }
};

} // namespace

std::unique_ptr<Gpu> makeGpuStub() { return std::make_unique<GpuStub>(); }

} // namespace sfcop::platform
