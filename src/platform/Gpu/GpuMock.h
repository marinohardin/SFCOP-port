// A Gpu for tests: records each call and returns the values the test programs. Every Status
// method returns `failWith` (success while it is kNone; with `failOnly` set, only the method of
// that name fails); the methods that return a handle or caps return their `...Result` member.
// Descriptions passed by reference are kept whole in `last...` members. ReadTexture() fills the
// region with `readValue` bytes. Render scales are kept per handle in `textureScales` (the handle
// CreateTexture() or ConfigureScaledBackBuffer() returned, mapped to the scale asked for); a call
// with a scale of 1 logs exactly as before render scales existed.
#pragma once

#include "platform/Core/MockLog.h"
#include "platform/Gpu/Gpu.h"

#include <cstring>
#include <map>
#include <string>
#include <string_view>

namespace sfcop::platform {

class GpuMock final : public Gpu {
public:
    MockLog log;

    Error failWith = Error::kNone;
    std::string failOnly; // when set, only the method of this name fails with `failWith`
    std::uint8_t readValue = 0;
    TextureDesc lastTextureDesc;
    PipelineDesc lastPipelineDesc;
    RenderTargets lastTargets;
    Viewport lastViewport;
    ClearDesc lastClear;
    DrawCall lastDraw;
    BlitDesc lastBlit;
    std::map<std::uint32_t, std::uint8_t> textureScales; // handle value -> render scale

    Result<GpuCaps> capsResult = GpuCaps{};
    Result<TextureId> backBufferResult = TextureId{1};
    Result<BufferId> createBufferResult = BufferId{1};
    Result<TextureId> createTextureResult = TextureId{2};
    Result<PaletteId> createPaletteResult = PaletteId{1};
    Result<PipelineId> createPipelineResult = PipelineId{1};

    Status Attach(Window &) override {
        log.Record("Attach");
        return status("Attach");
    }
    Status Detach() override {
        log.Record("Detach");
        return status("Detach");
    }
    Result<GpuCaps> GetCaps() override {
        log.Record("GetCaps");
        return capsResult;
    }
    Result<TextureId> ConfigureBackBuffer(std::uint32_t width, std::uint32_t height,
                                          PixelFormat format) override {
        log.Record("ConfigureBackBuffer", width, height, format);
        return remember(backBufferResult, 1);
    }
    Result<TextureId> ConfigureScaledBackBuffer(std::uint32_t width, std::uint32_t height,
                                                PixelFormat format,
                                                std::uint8_t renderScale) override {
        if (renderScale == 1) {
            return ConfigureBackBuffer(width, height, format);
        }
        log.Record("ConfigureScaledBackBuffer", width, height, format, renderScale);
        return remember(backBufferResult, renderScale);
    }
    Result<BufferId> CreateBuffer(BufferKind kind, std::uint32_t bytes) override {
        log.Record("CreateBuffer", kind, bytes);
        return createBufferResult;
    }
    Status WriteBuffer(BufferId buffer, std::uint32_t offset, const void *,
                       std::uint32_t bytes) override {
        log.Record("WriteBuffer", buffer, offset, bytes);
        return status("WriteBuffer");
    }
    Status DestroyBuffer(BufferId buffer) override {
        log.Record("DestroyBuffer", buffer);
        return status("DestroyBuffer");
    }
    Result<TextureId> CreateTexture(const TextureDesc &desc) override {
        if (desc.renderScale == 1) {
            log.Record("CreateTexture", desc.width, desc.height, desc.format);
        } else {
            log.Record("CreateTexture", desc.width, desc.height, desc.format, "scale",
                       desc.renderScale);
        }
        lastTextureDesc = desc;
        return remember(createTextureResult, desc.renderScale);
    }
    Status WriteTexture(TextureId texture, std::uint8_t level, Rect region, const void *,
                        std::uint32_t rowPitch) override {
        log.Record("WriteTexture", texture, level, region.x, region.y, region.width,
                   region.height, rowPitch);
        return status("WriteTexture");
    }
    Status ReadTexture(TextureId texture, std::uint8_t level, Rect region, void *out,
                       std::uint32_t rowPitch) override {
        log.Record("ReadTexture", texture, level, region.x, region.y, region.width,
                   region.height, rowPitch);
        if (!status("ReadTexture").Ok()) {
            return failWith;
        }
        std::memset(out, readValue, static_cast<std::size_t>(rowPitch) * region.height);
        return {};
    }
    Status DestroyTexture(TextureId texture) override {
        log.Record("DestroyTexture", texture);
        return status("DestroyTexture");
    }
    Status SetColorKey(TextureId texture, std::optional<std::uint32_t> key) override {
        if (key) {
            log.Record("SetColorKey", texture, *key);
        } else {
            log.Record("SetColorKey", texture, "none");
        }
        return status("SetColorKey");
    }
    Result<PaletteId> CreatePalette(const PaletteEntry *) override {
        log.Record("CreatePalette");
        return createPaletteResult;
    }
    Status WritePalette(PaletteId palette, std::uint32_t first, std::uint32_t count,
                        const PaletteEntry *) override {
        log.Record("WritePalette", palette, first, count);
        return status("WritePalette");
    }
    Status DestroyPalette(PaletteId palette) override {
        log.Record("DestroyPalette", palette);
        return status("DestroyPalette");
    }
    Status AttachPalette(TextureId texture, PaletteId palette) override {
        log.Record("AttachPalette", texture, palette);
        return status("AttachPalette");
    }
    Result<PipelineId> CreatePipeline(const PipelineDesc &desc) override {
        log.Record("CreatePipeline", desc.primitive, vertexStride(desc.layout));
        lastPipelineDesc = desc;
        return createPipelineResult;
    }
    Status DestroyPipeline(PipelineId pipeline) override {
        log.Record("DestroyPipeline", pipeline);
        return status("DestroyPipeline");
    }
    Status BeginScene(const RenderTargets &targets) override {
        log.Record("BeginScene", targets.color, targets.depth);
        lastTargets = targets;
        return status("BeginScene");
    }
    Status SetViewport(const Viewport &viewport) override {
        log.Record("SetViewport", viewport.x, viewport.y, viewport.width, viewport.height);
        lastViewport = viewport;
        return status("SetViewport");
    }
    Status Clear(const ClearDesc &clear) override {
        log.Record("Clear", clear.color, clear.depth);
        lastClear = clear;
        return status("Clear");
    }
    Status Draw(const DrawCall &draw) override {
        log.Record("Draw", draw.pipeline, draw.vertices, draw.firstVertex, draw.vertexCount,
                   draw.indices, draw.indexCount);
        lastDraw = draw;
        return status("Draw");
    }
    Status EndScene() override {
        log.Record("EndScene");
        return status("EndScene");
    }
    Status Blit(const BlitDesc &blit) override {
        log.Record("Blit", blit.source, blit.destination, blit.sourceColorKey);
        lastBlit = blit;
        return status("Blit");
    }
    Status Fill(TextureId texture, Rect area, std::uint32_t value) override {
        log.Record("Fill", texture, area.x, area.y, area.width, area.height, value);
        return status("Fill");
    }
    Status Present() override {
        log.Record("Present");
        return status("Present");
    }
    Status SetPresentFilter(TextureFilter filter) override {
        log.Record("SetPresentFilter", filter);
        return status("SetPresentFilter");
    }

private:
    Result<TextureId> remember(const Result<TextureId> &result, std::uint8_t renderScale) {
        if (result.Ok()) {
            textureScales[result.Value().value] = renderScale;
        }
        return result;
    }

    Status status(std::string_view method) const {
        if (failWith != Error::kNone && (failOnly.empty() || failOnly == method)) {
            return failWith;
        }
        return {};
    }
};

} // namespace sfcop::platform
