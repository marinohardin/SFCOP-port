#include "app/input/ScriptedGpu.h"

namespace sfcop::app::input {

using namespace platform;

Window &ScriptedGpu::AttachTarget(Window &given) const {
    return &given == &window_ ? window_.Inner() : given;
}
Status ScriptedGpu::Attach(Window &window) { return inner_.Attach(AttachTarget(window)); }
Status ScriptedGpu::Detach() { return inner_.Detach(); }
Result<GpuCaps> ScriptedGpu::GetCaps() { return inner_.GetCaps(); }
Result<TextureId> ScriptedGpu::ConfigureBackBuffer(std::uint32_t width, std::uint32_t height,
                                                   PixelFormat format) {
    return inner_.ConfigureBackBuffer(width, height, format);
}
Result<BufferId> ScriptedGpu::CreateBuffer(BufferKind kind, std::uint32_t bytes) {
    return inner_.CreateBuffer(kind, bytes);
}
Status ScriptedGpu::WriteBuffer(BufferId buffer, std::uint32_t offset, const void *data,
                                std::uint32_t bytes) {
    return inner_.WriteBuffer(buffer, offset, data, bytes);
}
Status ScriptedGpu::DestroyBuffer(BufferId buffer) { return inner_.DestroyBuffer(buffer); }
Result<TextureId> ScriptedGpu::CreateTexture(const TextureDesc &desc) {
    return inner_.CreateTexture(desc);
}
Status ScriptedGpu::WriteTexture(TextureId texture, std::uint8_t level, Rect region,
                                 const void *pixels, std::uint32_t rowPitch) {
    return inner_.WriteTexture(texture, level, region, pixels, rowPitch);
}
Status ScriptedGpu::ReadTexture(TextureId texture, std::uint8_t level, Rect region, void *out,
                                std::uint32_t rowPitch) {
    return inner_.ReadTexture(texture, level, region, out, rowPitch);
}
Status ScriptedGpu::DestroyTexture(TextureId texture) { return inner_.DestroyTexture(texture); }
Status ScriptedGpu::SetColorKey(TextureId texture, std::optional<std::uint32_t> key) {
    return inner_.SetColorKey(texture, key);
}
Result<PaletteId> ScriptedGpu::CreatePalette(const PaletteEntry *entries) {
    return inner_.CreatePalette(entries);
}
Status ScriptedGpu::WritePalette(PaletteId palette, std::uint32_t first, std::uint32_t count,
                                 const PaletteEntry *entries) {
    return inner_.WritePalette(palette, first, count, entries);
}
Status ScriptedGpu::DestroyPalette(PaletteId palette) { return inner_.DestroyPalette(palette); }
Status ScriptedGpu::AttachPalette(TextureId texture, PaletteId palette) {
    return inner_.AttachPalette(texture, palette);
}
Result<PipelineId> ScriptedGpu::CreatePipeline(const PipelineDesc &desc) {
    return inner_.CreatePipeline(desc);
}
Status ScriptedGpu::DestroyPipeline(PipelineId pipeline) { return inner_.DestroyPipeline(pipeline); }
Status ScriptedGpu::BeginScene(const RenderTargets &targets) { return inner_.BeginScene(targets); }
Status ScriptedGpu::SetViewport(const Viewport &viewport) { return inner_.SetViewport(viewport); }
Status ScriptedGpu::Clear(const ClearDesc &clear) { return inner_.Clear(clear); }
Status ScriptedGpu::Draw(const DrawCall &draw) { return inner_.Draw(draw); }
Status ScriptedGpu::EndScene() { return inner_.EndScene(); }
Status ScriptedGpu::Blit(const BlitDesc &blit) { return inner_.Blit(blit); }
Status ScriptedGpu::Fill(TextureId texture, Rect area, std::uint32_t value) {
    return inner_.Fill(texture, area, value);
}
Status ScriptedGpu::Present() { return inner_.Present(); }
Status ScriptedGpu::SetPresentFilter(platform::TextureFilter filter) {
    return inner_.SetPresentFilter(filter);
}

} // namespace sfcop::app::input
