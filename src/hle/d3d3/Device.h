// IDirect3DDevice3: Direct3D 3's fixed function on the Gpu (DESIGN §5.6).
//
// Render states and texture stage states are kept as the game sets them and turned into one Gpu
// PipelineDesc and DrawState at each draw; pipelines are kept by description. Vertices go through
// a stream buffer. The game's transformed and lit vertices (XYZRHW, diffuse, specular, one
// texture coordinate set) go to the Gpu unchanged; other formats are repacked, and untransformed
// positions are transformed on the CPU by world, view, projection and the viewport (no lighting,
// no clipping: the Gpu receives screen positions and 1/w, and vertices with w <= 0 are not
// clipped away). Stages 0 and 1 are drawn; a stage
// whose texture is unset passes the previous stage's colour on.
#pragma once

#include "hle/d3d3/D3d3Defs.h"
#include "hle/ddraw4/ComObject.h"

#include "platform/Gpu/Gpu.h"

#include <map>
#include <string>
#include <vector>

namespace sfcop::hle::ddraw4 {
class Surface;
} // namespace sfcop::hle::ddraw4

namespace sfcop::hle::d3d3 {

class Direct3D;
class Texture;
class Viewport;

constexpr HResult kD3dErrTooManyOperations = ddHresult(2077);

// D3DNEXT_* for NextViewport.
constexpr std::uint32_t kNextNext = 1;
constexpr std::uint32_t kNextHead = 2;
constexpr std::uint32_t kNextTail = 4;

constexpr std::uint32_t kLightStates = 9; // D3DLIGHTSTATETYPE 1-8

class Device final : public ddraw4::ComObject {
public:
    // Use Direct3D::CreateDevice(). The device holds a reference on its target.
    Device(Direct3D &owner, ddraw4::Surface &target);

    HResult GetCaps(DeviceDesc *hardware, DeviceDesc *software) const;

    // The viewport list. AddViewport and SetCurrentViewport hold a reference each, as the
    // original does; NextViewport hands one out without adding a reference.
    HResult AddViewport(Viewport *viewport);
    HResult DeleteViewport(Viewport *viewport);
    HResult NextViewport(Viewport *from, std::uint32_t flags, Viewport **out) const;
    HResult SetCurrentViewport(Viewport *viewport);
    HResult GetCurrentViewport(Viewport **out);

    // The texture formats the Gpu samples.
    HResult EnumTextureFormats(std::vector<ddraw4::PixelFormatDesc> &out) const;

    HResult BeginScene();
    HResult EndScene();

    HResult SetRenderTarget(ddraw4::Surface *target, std::uint32_t flags);
    HResult GetRenderTarget(ddraw4::Surface **out);

    HResult SetRenderState(std::uint32_t state, std::uint32_t value);
    HResult GetRenderState(std::uint32_t state, std::uint32_t &value) const;
    HResult SetLightState(std::uint32_t state, std::uint32_t value);
    HResult GetLightState(std::uint32_t state, std::uint32_t &value) const;
    HResult SetTransform(std::uint32_t type, const Matrix &matrix);
    HResult GetTransform(std::uint32_t type, Matrix &out) const;
    HResult MultiplyTransform(std::uint32_t type, const Matrix &matrix);

    HResult SetTexture(std::uint32_t stage, Texture *texture);
    HResult GetTexture(std::uint32_t stage, Texture **out);
    HResult SetTextureStageState(std::uint32_t stage, std::uint32_t state, std::uint32_t value);
    HResult GetTextureStageState(std::uint32_t stage, std::uint32_t state,
                                 std::uint32_t &value) const;

    // One pass for any state the Gpu can draw; otherwise the D3DERR_UNSUPPORTED... code for the
    // first stage operation or argument it cannot, or D3DERR_TOOMANYOPERATIONS past two stages.
    HResult ValidateDevice(std::uint32_t &passes) const;

    // `vertices` in the flexible vertex format `fvf`. D3DERR_SCENE_NOT_IN_SCENE outside a scene.
    HResult DrawPrimitive(std::uint32_t type, std::uint32_t fvf, const void *vertices,
                          std::uint32_t count, std::uint32_t flags);
    HResult DrawIndexedPrimitive(std::uint32_t type, std::uint32_t fvf, const void *vertices,
                                 std::uint32_t vertexCount, const std::uint16_t *indices,
                                 std::uint32_t indexCount, std::uint32_t flags);

    // Clears `rects` within `viewport`: on the Gpu inside a scene, as a surface fill outside one.
    // No rectangles clears the whole viewport. Inside a scene the Gpu viewport is set from
    // `viewport` for the clear (the Gpu clips to its own current one) and then put back to the
    // device's current viewport.
    HResult Clear(const Viewport &viewport, const std::vector<D3dRect> &rects, std::uint32_t flags,
                  std::uint32_t color, float z);

    // The pipeline and per-draw state the current states give for a draw.
    platform::PipelineDesc CurrentPipeline(const platform::VertexLayout &layout,
                                           platform::PrimitiveType primitive) const;
    platform::DrawState CurrentDrawState() const;

    Direct3D &Owner() const { return owner_; }
    ddraw4::Surface &Target() const { return *target_; }
    bool InScene() const { return inScene_; }

private:
    ~Device() override;

    platform::Gpu &gpu() const;
    void applyViewport(const Viewport *viewport);
    HResult clearRects(const std::vector<D3dRect> &rects, const ddraw4::RectL &limit,
                       std::uint32_t flags, std::uint32_t color, float depthValue,
                       ddraw4::Surface *depth);
    HResult draw(std::uint32_t type, std::uint32_t fvf, const void *vertices,
                 std::uint32_t vertexCount, const std::uint16_t *indices, std::uint32_t indexCount);
    platform::Result<std::uint32_t> stream(platform::BufferKind kind, const void *data,
                                           std::uint32_t bytes, std::uint32_t stride);
    platform::Result<platform::PipelineId> pipeline(const platform::PipelineDesc &desc);
    void applyLegacyState(std::uint32_t state, std::uint32_t value);

    Direct3D &owner_;
    ddraw4::Surface *target_;
    std::vector<Viewport *> viewports_;
    Viewport *current_ = nullptr;
    bool inScene_ = false;
    bool drawn_ = false;
    std::array<std::uint32_t, kRenderStates> renderState_{};
    std::array<std::array<std::uint32_t, kStageStates>, kStages> stageState_{};
    std::array<Texture *, kStages> textures_{};
    std::array<std::uint32_t, kLightStates> lightState_{};
    Matrix world_ = kIdentity;
    Matrix view_ = kIdentity;
    Matrix projection_ = kIdentity;
    std::map<std::string, platform::PipelineId> pipelines_;
    struct Stream {
        platform::BufferId buffer;
        std::uint32_t capacity = 0;
        std::uint32_t used = 0;
    };
    Stream vertexStream_;
    Stream indexStream_;
};

} // namespace sfcop::hle::d3d3
