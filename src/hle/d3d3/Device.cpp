#include "hle/d3d3/Device.h"
#include "hle/d3d3/Direct3D.h"
#include "hle/d3d3/Texture.h"
#include "hle/d3d3/Vertices.h"
#include "hle/d3d3/Viewport.h"
#include "hle/ddraw4/DirectDraw.h"
#include "hle/ddraw4/Surface.h"

#include <algorithm>
#include <cstring>
#include <optional>

namespace sfcop::hle::d3d3 {

using platform::BlendFactor;
using platform::CompareFunction;
using platform::PixelFormat;
using platform::TextureArg;
using platform::TextureOp;

namespace {

constexpr std::uint32_t kVertexStreamBytes = 1u << 20;
constexpr std::uint32_t kIndexStreamBytes = 1u << 18;
constexpr std::uint32_t kBlendBothSrcAlpha = 12;
constexpr std::uint32_t kBlendBothInvSrcAlpha = 13;

std::uint32_t bitsOf(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

float floatOf(std::uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

CompareFunction compare(std::uint32_t value) {
    if (value < 1 || value > 8) {
        return CompareFunction::kAlways;
    }
    return static_cast<CompareFunction>(value - 1); // D3DCMP_NEVER .. ALWAYS, in Gpu order
}

BlendFactor blend(std::uint32_t value, BlendFactor fallback) {
    if (value < 1 || value > 11) {
        return fallback;
    }
    return static_cast<BlendFactor>(value - 1); // D3DBLEND_ZERO .. SRCALPHASAT, in Gpu order
}

std::optional<TextureOp> textureOp(std::uint32_t value) {
    switch (value) {
    case kTopDisable:
        return TextureOp::kDisable;
    case kTopSelectArg1:
        return TextureOp::kSelectArg1;
    case kTopSelectArg2:
        return TextureOp::kSelectArg2;
    case kTopModulate:
        return TextureOp::kModulate;
    case kTopModulate2x:
        return TextureOp::kModulate2x;
    case kTopModulate4x:
        return TextureOp::kModulate4x;
    case kTopAdd:
        return TextureOp::kAdd;
    case kTopAddSigned:
        return TextureOp::kAddSigned;
    case kTopSubtract:
        return TextureOp::kSubtract;
    case kTopBlendDiffuseAlpha:
        return TextureOp::kBlendDiffuseAlpha;
    case kTopBlendTextureAlpha:
        return TextureOp::kBlendTextureAlpha;
    case kTopBlendFactorAlpha:
        return TextureOp::kBlendFactorAlpha;
    case kTopBlendCurrentAlpha:
        return TextureOp::kBlendCurrentAlpha;
    default:
        return std::nullopt;
    }
}

std::optional<TextureArg> textureArg(std::uint32_t value) {
    switch (value) { // a modifier (complement, alpha replicate) is not in this list
    case kTaDiffuse:
        return TextureArg::kDiffuse;
    case kTaCurrent:
        return TextureArg::kCurrent;
    case kTaTexture:
        return TextureArg::kTexture;
    case kTaTFactor:
        return TextureArg::kTextureFactor;
    case kTaSpecular:
        return TextureArg::kSpecular;
    default:
        return std::nullopt;
    }
}

platform::TextureAddress address(std::uint32_t value) {
    switch (value) {
    case 2:
        return platform::TextureAddress::kMirror;
    case 3:
    case 4: // border: clamped, the Gpu has no border colour
        return platform::TextureAddress::kClamp;
    default:
        return platform::TextureAddress::kWrap;
    }
}

platform::TextureFilter filter(std::uint32_t value) {
    return value == 1 ? platform::TextureFilter::kPoint : platform::TextureFilter::kLinear;
}

platform::MipFilter mipFilter(std::uint32_t value) {
    switch (value) {
    case 2:
        return platform::MipFilter::kPoint;
    case 3:
        return platform::MipFilter::kLinear;
    default:
        return platform::MipFilter::kNone;
    }
}

Matrix multiply(const Matrix &a, const Matrix &b) {
    Matrix c{};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a[i * 4 + k] * b[k * 4 + j];
            }
            c[i * 4 + j] = sum;
        }
    }
    return c;
}

// Vertices that make whole primitives of `type`: the rest are dropped, as Direct3D draws only
// whole ones.
std::uint32_t wholePrimitives(std::uint32_t type, std::uint32_t count) {
    switch (type) {
    case 2:
        return count - count % 2;
    case 3:
        return count >= 2 ? count : 0;
    case 4:
        return count - count % 3;
    case 5:
    case 6:
        return count >= 3 ? count : 0;
    default:
        return count;
    }
}

void append(std::string &key, std::uint32_t value) {
    key.append(reinterpret_cast<const char *>(&value), sizeof value);
}

// The pipeline's description as bytes, for the cache.
std::string pipelineKey(const platform::PipelineDesc &desc) {
    std::string key;
    const auto &layout = desc.layout;
    for (std::uint32_t value : {std::uint32_t(layout.position),
                                std::uint32_t(layout.normal),
                                std::uint32_t(layout.diffuse),
                                std::uint32_t(layout.specular),
                                std::uint32_t(layout.texCoordSets),
                                std::uint32_t(desc.primitive),
                                std::uint32_t(desc.colorFormat),
                                std::uint32_t(desc.depthTarget),
                                std::uint32_t(desc.blend),
                                std::uint32_t(desc.sourceBlend),
                                std::uint32_t(desc.destinationBlend),
                                std::uint32_t(desc.depthTest),
                                std::uint32_t(desc.depthWrite),
                                std::uint32_t(desc.depthFunction),
                                std::uint32_t(desc.alphaTest),
                                std::uint32_t(desc.alphaFunction),
                                std::uint32_t(desc.cull),
                                std::uint32_t(desc.fill),
                                std::uint32_t(desc.shade),
                                std::uint32_t(desc.specular),
                                std::uint32_t(desc.colorKey),
                                std::uint32_t(desc.fog)}) {
        append(key, value);
    }
    for (const platform::TextureStage &stage : desc.stages) {
        for (std::uint32_t value :
             {std::uint32_t(stage.colorOp), std::uint32_t(stage.colorArg1),
              std::uint32_t(stage.colorArg2), std::uint32_t(stage.alphaOp),
              std::uint32_t(stage.alphaArg1), std::uint32_t(stage.alphaArg2),
              std::uint32_t(stage.addressU), std::uint32_t(stage.addressV),
              std::uint32_t(stage.magFilter), std::uint32_t(stage.minFilter),
              std::uint32_t(stage.mipFilter), std::uint32_t(stage.texCoordIndex)}) {
            append(key, value);
        }
    }
    return key;
}

ddraw4::RectL intersect(const ddraw4::RectL &a, const ddraw4::RectL &b) {
    return ddraw4::RectL{std::max(a.left, b.left), std::max(a.top, b.top),
                         std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
}

} // namespace

Device::Device(Direct3D &owner, ddraw4::Surface &target) : owner_(owner), target_(&target) {
    owner_.AdoptChild();
    target_->AddRef(); // the device draws into it until it is replaced or the device dies

    // Direct3D's defaults.
    renderState_[kRsTextureAddress] = 1;
    renderState_[4] = 1; // TEXTUREPERSPECTIVE
    renderState_[kRsZEnable] = target.ZBuffer() != nullptr ? 1 : 0;
    renderState_[kRsFillMode] = 3;
    renderState_[kRsShadeMode] = 2;
    renderState_[kRsZWriteEnable] = 1;
    renderState_[kRsTextureMag] = 1;
    renderState_[kRsTextureMin] = 1;
    renderState_[kRsSrcBlend] = 2;
    renderState_[kRsDestBlend] = 1;
    renderState_[kRsTextureMapBlend] = kTblendModulate;
    renderState_[kRsCullMode] = 3;
    renderState_[kRsZFunc] = 4;
    renderState_[kRsAlphaFunc] = 8;
    renderState_[kRsFogTableEnd] = bitsOf(1.0f);
    renderState_[kRsFogTableDensity] = bitsOf(1.0f);
    renderState_[kRsTextureAddressU] = 1;
    renderState_[kRsTextureAddressV] = 1;
    renderState_[kRsTextureFactor] = 0xFFFFFFFF;
    for (std::uint32_t stage = 0; stage < kStages; ++stage) {
        auto &state = stageState_[stage];
        state[kTssColorOp] = stage == 0 ? kTopModulate : kTopDisable;
        state[kTssColorArg1] = kTaTexture;
        state[kTssColorArg2] = kTaCurrent;
        state[kTssAlphaOp] = stage == 0 ? kTopSelectArg1 : kTopDisable;
        state[kTssAlphaArg1] = kTaTexture;
        state[kTssAlphaArg2] = kTaCurrent;
        state[kTssTexCoordIndex] = stage;
        state[kTssAddress] = 1;
        state[kTssAddressU] = 1;
        state[kTssAddressV] = 1;
        state[kTssMagFilter] = 1;
        state[kTssMinFilter] = 1;
        state[kTssMipFilter] = 1;
    }
}

Device::~Device() {
    if (inScene_) {
        (void)gpu().EndScene();
    }
    for (Texture *texture : textures_) {
        if (texture != nullptr) {
            texture->Release();
        }
    }
    if (current_ != nullptr) {
        current_->Release();
    }
    for (Viewport *viewport : viewports_) {
        viewport->AttachDevice(nullptr);
        viewport->Release();
    }
    for (const auto &entry : pipelines_) {
        (void)gpu().DestroyPipeline(entry.second);
    }
    for (Stream *stream : {&vertexStream_, &indexStream_}) {
        if (stream->buffer.Valid()) {
            (void)gpu().DestroyBuffer(stream->buffer);
        }
    }
    target_->Release();
    owner_.ChildGone(); // last: it may destroy the Direct3D object
}

platform::Gpu &Device::gpu() const {
    return owner_.Owner().Device();
}

HResult Device::GetCaps(DeviceDesc *hardware, DeviceDesc *software) const {
    if (hardware == nullptr && software == nullptr) {
        return ddraw4::kDdErrInvalidParams;
    }
    if (hardware != nullptr) {
        *hardware = owner_.HalDevice().hardware;
    }
    if (software != nullptr) {
        *software = DeviceDesc{};
    }
    return kDdOk;
}

HResult Device::AddViewport(Viewport *viewport) {
    if (viewport == nullptr || viewport->AttachedDevice() != nullptr) {
        return ddraw4::kDdErrInvalidParams;
    }
    viewport->AddRef(); // the list's reference
    viewport->AttachDevice(this);
    viewports_.push_back(viewport);
    return kDdOk;
}

HResult Device::DeleteViewport(Viewport *viewport) {
    const auto found = std::find(viewports_.begin(), viewports_.end(), viewport);
    if (found == viewports_.end()) {
        return ddraw4::kDdErrInvalidParams;
    }
    viewports_.erase(found);
    if (current_ == viewport) {
        current_ = nullptr;
        viewport->Release();
    }
    viewport->AttachDevice(nullptr);
    viewport->Release();
    return kDdOk;
}

HResult Device::NextViewport(Viewport *from, std::uint32_t flags, Viewport **out) const {
    if (out == nullptr) {
        return ddraw4::kDdErrInvalidParams;
    }
    *out = nullptr;
    if (viewports_.empty()) {
        return ddraw4::kDdErrNotFound;
    }
    if (flags == kNextHead) {
        *out = viewports_.front();
    } else if (flags == kNextTail) {
        *out = viewports_.back();
    } else if (flags == kNextNext) {
        const auto found = std::find(viewports_.begin(), viewports_.end(), from);
        if (found == viewports_.end()) {
            return ddraw4::kDdErrInvalidParams;
        }
        if (found + 1 == viewports_.end()) {
            return ddraw4::kDdErrNotFound;
        }
        *out = *(found + 1);
    } else {
        return ddraw4::kDdErrInvalidParams;
    }
    return kDdOk;
}

HResult Device::SetCurrentViewport(Viewport *viewport) {
    if (viewport == nullptr ||
        std::find(viewports_.begin(), viewports_.end(), viewport) == viewports_.end()) {
        return kD3dErrInvalidCurrentViewport;
    }
    viewport->AddRef(); // the current viewport's reference
    if (current_ != nullptr) {
        current_->Release();
    }
    current_ = viewport;
    return kDdOk;
}

HResult Device::GetCurrentViewport(Viewport **out) {
    if (out == nullptr) {
        return ddraw4::kDdErrInvalidParams;
    }
    *out = current_;
    if (current_ == nullptr) {
        return kD3dErrNoCurrentViewport;
    }
    current_->AddRef();
    return kDdOk;
}

HResult Device::EnumTextureFormats(std::vector<ddraw4::PixelFormatDesc> &out) const {
    out.clear();
    // The fallback when the Gpu does not list its formats: what the Gpu can always hold.
    std::vector<PixelFormat> formats{PixelFormat::kRgb565, PixelFormat::kArgb1555,
                                     PixelFormat::kArgb8888, PixelFormat::kXrgb8888,
                                     PixelFormat::kIndexed8};
    platform::Result<platform::GpuCaps> caps = gpu().GetCaps();
    if (caps.Ok() && !caps.Value().textureFormats.empty()) {
        formats = caps.Value().textureFormats;
    }
    for (PixelFormat format : formats) {
        if (format != PixelFormat::kDepth16) {
            out.push_back(ddraw4::describe(format));
        }
    }
    return kDdOk;
}

// Sets the Gpu viewport from a Direct3D viewport's rectangle (clipped to the target) and depth
// range. A viewport with no data, or one wholly outside the target, leaves the Gpu's as it is.
void Device::applyViewport(const Viewport *viewport) {
    if (viewport == nullptr || !viewport->Desc()) {
        return;
    }
    const ViewportDesc &desc = *viewport->Desc();
    const ddraw4::RectL area = intersect(
        ddraw4::RectL{static_cast<std::int32_t>(desc.x), static_cast<std::int32_t>(desc.y),
                      static_cast<std::int32_t>(desc.x + desc.width),
                      static_cast<std::int32_t>(desc.y + desc.height)},
        ddraw4::RectL{0, 0, static_cast<std::int32_t>(target_->Width()),
                      static_cast<std::int32_t>(target_->Height())});
    if (area.left >= area.right || area.top >= area.bottom) {
        return;
    }
    platform::Viewport gpuViewport;
    gpuViewport.x = area.left;
    gpuViewport.y = area.top;
    gpuViewport.width = static_cast<std::uint32_t>(area.right - area.left);
    gpuViewport.height = static_cast<std::uint32_t>(area.bottom - area.top);
    gpuViewport.minDepth = std::clamp(desc.minZ, 0.0f, 1.0f);
    gpuViewport.maxDepth = std::clamp(desc.maxZ, gpuViewport.minDepth, 1.0f);
    (void)gpu().SetViewport(gpuViewport);
}

HResult Device::BeginScene() {
    if (inScene_) {
        return kD3dErrSceneInScene;
    }
    // The scene's targets at the render scale the window asks for now (fix_render_scale): a
    // resize since the last scene makes them again here, at the next scene's start.
    const std::uint8_t scale = target_->Owner().BeginSceneScale();
    target_->UseRenderScale(scale);
    if (ddraw4::Surface *z = target_->ZBuffer()) {
        z->UseRenderScale(scale);
    }
    platform::Result<platform::TextureId> color = target_->GpuTexture();
    if (!color.Ok()) {
        return ddraw4::fromError(color.Code());
    }
    platform::RenderTargets targets;
    targets.color = color.Value();
    if (ddraw4::Surface *z = target_->ZBuffer()) {
        platform::Result<platform::TextureId> depth = z->GpuTexture();
        if (!depth.Ok()) {
            return ddraw4::fromError(depth.Code());
        }
        targets.depth = depth.Value();
    }
    const platform::Status begun = gpu().BeginScene(targets);
    if (!begun.Ok()) {
        return ddraw4::fromError(begun.Code());
    }
    inScene_ = true;
    drawn_ = false;
    applyViewport(current_);
    return kDdOk;
}

HResult Device::EndScene() {
    if (!inScene_) {
        return kD3dErrSceneNotInScene;
    }
    inScene_ = false;
    const platform::Status ended = gpu().EndScene();
    if (drawn_) {
        target_->MarkGpuWritten();
        if (ddraw4::Surface *z = target_->ZBuffer()) {
            z->MarkGpuWritten();
        }
    }
    return ended.Ok() ? kDdOk : ddraw4::fromError(ended.Code());
}

HResult Device::SetRenderTarget(ddraw4::Surface *target, std::uint32_t) {
    if (target == nullptr) {
        return ddraw4::kDdErrInvalidParams;
    }
    if (inScene_) {
        return kD3dErrSceneInScene;
    }
    ddraw4::SurfaceCaps caps;
    (void)target->GetCaps(caps);
    if ((caps.caps & ddraw4::kDdscaps3dDevice) == 0) {
        return ddraw4::kDdErrInvalidCaps;
    }
    target->AddRef();
    target_->Release();
    target_ = target;
    return kDdOk;
}

HResult Device::GetRenderTarget(ddraw4::Surface **out) {
    if (out == nullptr) {
        return ddraw4::kDdErrInvalidParams;
    }
    target_->AddRef();
    *out = target_;
    return kDdOk;
}

HResult Device::SetRenderState(std::uint32_t state, std::uint32_t value) {
    if (state == 0 || state >= kRenderStates) {
        return ddraw4::kDdErrInvalidParams;
    }
    renderState_[state] = value;
    applyLegacyState(state, value);
    return kDdOk;
}

HResult Device::GetRenderState(std::uint32_t state, std::uint32_t &value) const {
    if (state == 0 || state >= kRenderStates) {
        return ddraw4::kDdErrInvalidParams;
    }
    value = renderState_[state];
    return kDdOk;
}

// The Direct3D 5 states that set texture stage 0 (a texture handle, the blend, filters and
// addressing), as Direct3D 6 maps them.
void Device::applyLegacyState(std::uint32_t state, std::uint32_t value) {
    auto &stage = stageState_[0];
    switch (state) {
    case kRsTextureHandle:
    {
        Texture *named = Texture::FromHandle(value);
        (void)SetTexture(0, named);
        if (named != nullptr) {
            named->Release(); // the stage holds its own reference
        }
        break;
    }
    case kRsTextureMapBlend:
        stage[kTssColorArg1] = kTaTexture;
        stage[kTssColorArg2] = kTaCurrent;
        stage[kTssAlphaArg1] = kTaTexture;
        stage[kTssAlphaArg2] = kTaCurrent;
        switch (value) {
        case kTblendDecal:
        case kTblendCopy:
        case 5: // DECALMASK
            stage[kTssColorOp] = kTopSelectArg1;
            stage[kTssAlphaOp] = kTopSelectArg1;
            break;
        case kTblendModulateAlpha:
            stage[kTssColorOp] = kTopModulate;
            stage[kTssAlphaOp] = kTopModulate;
            break;
        case kTblendDecalAlpha:
            stage[kTssColorOp] = kTopBlendTextureAlpha;
            stage[kTssAlphaOp] = kTopSelectArg2;
            break;
        case kTblendAdd:
            stage[kTssColorOp] = kTopAdd;
            stage[kTssAlphaOp] = kTopSelectArg2;
            break;
        default: // MODULATE, MODULATEMASK
            stage[kTssColorOp] = kTopModulate;
            stage[kTssAlphaOp] = kTopSelectArg1;
            break;
        }
        break;
    case kRsTextureMag:
        stage[kTssMagFilter] = (value == 1 || value == 3 || value == 4) ? 1 : 2;
        break;
    case kRsTextureMin:
        stage[kTssMinFilter] = (value == 1 || value == 3 || value == 4) ? 1 : 2;
        stage[kTssMipFilter] = (value == 3 || value == 5) ? 2 : (value == 4 || value == 6) ? 3 : 1;
        break;
    case kRsTextureAddress:
        stage[kTssAddressU] = value;
        stage[kTssAddressV] = value;
        break;
    case kRsTextureAddressU:
        stage[kTssAddressU] = value;
        break;
    case kRsTextureAddressV:
        stage[kTssAddressV] = value;
        break;
    default:
        break;
    }
}

HResult Device::SetLightState(std::uint32_t state, std::uint32_t value) {
    if (state == 0 || state >= kLightStates) {
        return ddraw4::kDdErrInvalidParams;
    }
    lightState_[state] = value; // kept; vertices are drawn as the game lit them
    return kDdOk;
}

HResult Device::GetLightState(std::uint32_t state, std::uint32_t &value) const {
    if (state == 0 || state >= kLightStates) {
        return ddraw4::kDdErrInvalidParams;
    }
    value = lightState_[state];
    return kDdOk;
}

HResult Device::SetTransform(std::uint32_t type, const Matrix &matrix) {
    switch (type) {
    case kTransformWorld:
        world_ = matrix;
        return kDdOk;
    case kTransformView:
        view_ = matrix;
        return kDdOk;
    case kTransformProjection:
        projection_ = matrix;
        return kDdOk;
    default:
        return ddraw4::kDdErrInvalidParams;
    }
}

HResult Device::GetTransform(std::uint32_t type, Matrix &out) const {
    switch (type) {
    case kTransformWorld:
        out = world_;
        return kDdOk;
    case kTransformView:
        out = view_;
        return kDdOk;
    case kTransformProjection:
        out = projection_;
        return kDdOk;
    default:
        return ddraw4::kDdErrInvalidParams;
    }
}

HResult Device::MultiplyTransform(std::uint32_t type, const Matrix &matrix) {
    Matrix current;
    const HResult got = GetTransform(type, current);
    if (got != kDdOk) {
        return got;
    }
    return SetTransform(type, multiply(matrix, current)); // the given matrix first
}

HResult Device::SetTexture(std::uint32_t stage, Texture *texture) {
    if (stage >= kStages) {
        return ddraw4::kDdErrInvalidParams;
    }
    if (texture != nullptr) {
        texture->AddRef(); // the stage's reference
    }
    if (textures_[stage] != nullptr) {
        textures_[stage]->Release();
    }
    textures_[stage] = texture;
    return kDdOk;
}

HResult Device::GetTexture(std::uint32_t stage, Texture **out) {
    if (stage >= kStages || out == nullptr) {
        return ddraw4::kDdErrInvalidParams;
    }
    *out = textures_[stage];
    if (*out != nullptr) {
        (*out)->AddRef();
    }
    return kDdOk;
}

HResult Device::SetTextureStageState(std::uint32_t stage, std::uint32_t state,
                                     std::uint32_t value) {
    if (stage >= kStages || state == 0 || state >= kStageStates) {
        return ddraw4::kDdErrInvalidParams;
    }
    stageState_[stage][state] = value;
    return kDdOk;
}

HResult Device::GetTextureStageState(std::uint32_t stage, std::uint32_t state,
                                     std::uint32_t &value) const {
    if (stage >= kStages || state == 0 || state >= kStageStates) {
        return ddraw4::kDdErrInvalidParams;
    }
    value = stageState_[stage][state];
    return kDdOk;
}

HResult Device::ValidateDevice(std::uint32_t &passes) const {
    passes = 0;
    for (std::uint32_t stage = 0; stage < kStages; ++stage) {
        const auto &state = stageState_[stage];
        if (state[kTssColorOp] == kTopDisable) {
            break;
        }
        if (stage >= platform::kTextureStages) {
            return kD3dErrTooManyOperations;
        }
        if (!textureOp(state[kTssColorOp])) {
            return kD3dErrUnsupportedColorOperation;
        }
        if (!textureArg(state[kTssColorArg1]) || !textureArg(state[kTssColorArg2])) {
            return kD3dErrUnsupportedColorArg;
        }
        if (!textureOp(state[kTssAlphaOp])) {
            return kD3dErrUnsupportedAlphaOperation;
        }
        if (!textureArg(state[kTssAlphaArg1]) || !textureArg(state[kTssAlphaArg2])) {
            return kD3dErrUnsupportedAlphaArg;
        }
    }
    passes = 1;
    return kDdOk;
}

platform::PipelineDesc Device::CurrentPipeline(const platform::VertexLayout &layout,
                                               platform::PrimitiveType primitive) const {
    const auto &rs = renderState_;
    platform::PipelineDesc desc;
    desc.layout = layout;
    desc.primitive = primitive;
    desc.colorFormat = target_->Format();
    desc.depthTarget = target_->ZBuffer() != nullptr;
    desc.depthTest = desc.depthTarget && rs[kRsZEnable] != 0;
    desc.depthWrite = rs[kRsZWriteEnable] != 0;
    desc.depthFunction = compare(rs[kRsZFunc]);
    desc.alphaTest = rs[kRsAlphaTestEnable] != 0;
    desc.alphaFunction = compare(rs[kRsAlphaFunc]);
    desc.blend = rs[kRsAlphaBlendEnable] != 0;
    if (rs[kRsSrcBlend] == kBlendBothSrcAlpha || rs[kRsSrcBlend] == kBlendBothInvSrcAlpha) {
        const bool inverse = rs[kRsSrcBlend] == kBlendBothInvSrcAlpha;
        desc.sourceBlend = inverse ? BlendFactor::kInverseSourceAlpha : BlendFactor::kSourceAlpha;
        desc.destinationBlend =
            inverse ? BlendFactor::kSourceAlpha : BlendFactor::kInverseSourceAlpha;
    } else {
        desc.sourceBlend = blend(rs[kRsSrcBlend], BlendFactor::kOne);
        desc.destinationBlend = blend(rs[kRsDestBlend], BlendFactor::kZero);
    }
    switch (rs[kRsCullMode]) {
    case 1:
        desc.cull = platform::CullMode::kNone;
        break;
    case 2:
        desc.cull = platform::CullMode::kClockwise;
        break;
    default:
        desc.cull = platform::CullMode::kCounterClockwise;
        break;
    }
    desc.fill = rs[kRsFillMode] == 1   ? platform::FillMode::kPoint
                : rs[kRsFillMode] == 2 ? platform::FillMode::kWireframe
                                       : platform::FillMode::kSolid;
    desc.shade = rs[kRsShadeMode] == 1 ? platform::ShadeMode::kFlat : platform::ShadeMode::kGouraud;
    desc.specular = rs[kRsSpecularEnable] != 0;
    desc.colorKey = rs[kRsColorKeyEnable] != 0;
    desc.fog = platform::FogMode::kNone;
    if (rs[kRsFogEnable] != 0) {
        switch (rs[kRsFogTableMode]) {
        case 1:
            desc.fog = platform::FogMode::kExp;
            break;
        case 2:
            desc.fog = platform::FogMode::kExp2;
            break;
        case 3:
            desc.fog = platform::FogMode::kLinear;
            break;
        default:
            desc.fog = platform::FogMode::kVertex;
            break;
        }
    }

    bool disabled = false;
    for (std::size_t s = 0; s < platform::kTextureStages; ++s) {
        const auto &state = stageState_[s];
        platform::TextureStage &stage = desc.stages[s];
        disabled = disabled || state[kTssColorOp] == kTopDisable;
        if (disabled) {
            stage.colorOp = TextureOp::kDisable;
            stage.alphaOp = TextureOp::kDisable;
            continue;
        }
        stage.colorOp = textureOp(state[kTssColorOp]).value_or(TextureOp::kModulate);
        stage.colorArg1 =
            textureArg(state[kTssColorArg1] & kTaSelectMask).value_or(TextureArg::kTexture);
        stage.colorArg2 =
            textureArg(state[kTssColorArg2] & kTaSelectMask).value_or(TextureArg::kCurrent);
        stage.alphaOp = textureOp(state[kTssAlphaOp]).value_or(TextureOp::kSelectArg1);
        stage.alphaArg1 =
            textureArg(state[kTssAlphaArg1] & kTaSelectMask).value_or(TextureArg::kTexture);
        stage.alphaArg2 =
            textureArg(state[kTssAlphaArg2] & kTaSelectMask).value_or(TextureArg::kCurrent);
        if (stage.alphaOp == TextureOp::kDisable) {
            // Alpha off while colour is on: the previous stage's alpha passes through.
            stage.alphaOp = TextureOp::kSelectArg1;
            stage.alphaArg1 = TextureArg::kCurrent;
        }
        if (textures_[s] == nullptr || layout.texCoordSets == 0) {
            // No texture: the stage passes the previous colour on where it would sample.
            for (TextureArg *arg :
                 {&stage.colorArg1, &stage.colorArg2, &stage.alphaArg1, &stage.alphaArg2}) {
                if (*arg == TextureArg::kTexture) {
                    *arg = TextureArg::kCurrent;
                }
            }
        }
        stage.addressU = address(state[kTssAddressU]);
        stage.addressV = address(state[kTssAddressV]);
        stage.magFilter = filter(state[kTssMagFilter]);
        stage.minFilter = filter(state[kTssMinFilter]);
        stage.mipFilter = mipFilter(state[kTssMipFilter]);
        const std::uint32_t index = state[kTssTexCoordIndex] & 0xFFFF;
        stage.texCoordIndex = static_cast<std::uint8_t>(
            layout.texCoordSets == 0 ? 0
                                     : std::min<std::uint32_t>(index, layout.texCoordSets - 1u));
    }
    return desc;
}

platform::DrawState Device::CurrentDrawState() const {
    const auto &rs = renderState_;
    platform::DrawState state;
    state.fogColor = rs[kRsFogColor] & 0x00FFFFFF;
    state.fogStart = floatOf(rs[kRsFogTableStart]);
    state.fogEnd = floatOf(rs[kRsFogTableEnd]);
    state.fogDensity = floatOf(rs[kRsFogTableDensity]);
    state.alphaReference = static_cast<std::uint8_t>(rs[kRsAlphaRef] & 0xFF);
    state.textureFactor = rs[kRsTextureFactor];
    return state;
}

platform::Result<platform::PipelineId> Device::pipeline(const platform::PipelineDesc &desc) {
    const std::string key = pipelineKey(desc);
    const auto found = pipelines_.find(key);
    if (found != pipelines_.end()) {
        return found->second;
    }
    platform::Result<platform::PipelineId> made = gpu().CreatePipeline(desc);
    if (made.Ok()) {
        pipelines_.emplace(key, made.Value());
    }
    return made;
}

platform::Result<std::uint32_t> Device::stream(platform::BufferKind kind, const void *data,
                                               std::uint32_t bytes, std::uint32_t stride) {
    Stream &stream = kind == platform::BufferKind::kVertex ? vertexStream_ : indexStream_;
    if (!stream.buffer.Valid() || stream.capacity < bytes) {
        if (stream.buffer.Valid()) {
            (void)gpu().DestroyBuffer(stream.buffer);
            stream.buffer = {};
        }
        const std::uint32_t minimum =
            kind == platform::BufferKind::kVertex ? kVertexStreamBytes : kIndexStreamBytes;
        const std::uint32_t capacity = std::max(minimum, (bytes + 3u) & ~3u);
        platform::Result<platform::BufferId> made = gpu().CreateBuffer(kind, capacity);
        if (!made.Ok()) {
            return made.Code();
        }
        stream = Stream{made.Value(), capacity, 0};
    }
    // Each draw's data starts on a whole vertex (or index) of the buffer.
    std::uint32_t offset = (stream.used + stride - 1) / stride * stride;
    if (offset + bytes > stream.capacity) {
        offset = 0; // draws already recorded keep the data they saw
    }
    const platform::Status written = gpu().WriteBuffer(stream.buffer, offset, data, bytes);
    if (!written.Ok()) {
        return written.Code();
    }
    stream.used = offset + bytes;
    return offset / stride;
}

HResult Device::DrawPrimitive(std::uint32_t type, std::uint32_t fvf, const void *vertices,
                              std::uint32_t count, std::uint32_t) {
    return draw(type, fvf, vertices, count, nullptr, 0);
}

HResult Device::DrawIndexedPrimitive(std::uint32_t type, std::uint32_t fvf, const void *vertices,
                                     std::uint32_t vertexCount, const std::uint16_t *indices,
                                     std::uint32_t indexCount, std::uint32_t) {
    if (indices == nullptr) {
        return ddraw4::kDdErrInvalidParams;
    }
    return draw(type, fvf, vertices, vertexCount, indices, indexCount);
}

HResult Device::draw(std::uint32_t type, std::uint32_t fvf, const void *vertices,
                     std::uint32_t vertexCount, const std::uint16_t *indices,
                     std::uint32_t indexCount) {
    if (!inScene_) {
        return kD3dErrSceneNotInScene;
    }
    if (type < kPtPointList || type > kPtTriangleFan) {
        return kD3dErrInvalidPrimitiveType;
    }
    if (vertices == nullptr) {
        return ddraw4::kDdErrInvalidParams;
    }
    const std::optional<VertexPlan> plan = planVertices(fvf);
    if (!plan) {
        return kD3dErrInvalidVertexType;
    }
    std::uint32_t drawn = wholePrimitives(type, indices != nullptr ? indexCount : vertexCount);
    if (drawn == 0 || vertexCount == 0) {
        return kDdOk;
    }

    const std::uint8_t *data = static_cast<const std::uint8_t *>(vertices);
    std::vector<std::uint8_t> repacked;
    if (!plan->passThrough) {
        ViewportDesc viewport;
        if (!plan->transformed) {
            if (current_ == nullptr || !current_->Desc()) {
                return kD3dErrNoCurrentViewport;
            }
            viewport = *current_->Desc();
        }
        repacked = repackVertices(*plan, data, vertexCount,
                                  multiply(multiply(world_, view_), projection_), viewport);
        data = repacked.data();
    }

    auto primitive = static_cast<platform::PrimitiveType>(type - 1); // D3DPT order
    const std::uint32_t stride = platform::vertexStride(plan->layout);
    std::vector<std::uint8_t> widened;
    const bool widen = target_->RenderScale() > 1 &&
                       plan->layout.position == platform::PositionKind::kXyzRhw &&
                       (primitive == platform::PrimitiveType::kPointList ||
                        primitive == platform::PrimitiveType::kLineList ||
                        primitive == platform::PrimitiveType::kLineStrip);
    if (widen) {
        widened = widenPrimitives(primitive, data, stride, indices, drawn);
        if (widened.empty()) {
            return kDdOk;
        }
        data = widened.data();
        vertexCount = static_cast<std::uint32_t>(widened.size() / stride);
        drawn = vertexCount;
        indices = nullptr;
        primitive = platform::PrimitiveType::kTriangleList;
    }
    platform::PipelineDesc desc = CurrentPipeline(plan->layout, primitive);
    if (widen) {
        desc.cull = platform::CullMode::kNone; // the triangles' winding is ours, not the game's
    }
    platform::DrawState state = CurrentDrawState();
    for (std::size_t s = 0; s < platform::kTextureStages; ++s) {
        if (desc.stages[s].colorOp != TextureOp::kDisable && textures_[s] != nullptr &&
            plan->layout.texCoordSets != 0) {
            platform::Result<platform::TextureId> texture = textures_[s]->Surface().GpuTexture();
            if (texture.Ok()) {
                state.textures[s] = texture.Value();
            }
        }
    }
    platform::Result<platform::PipelineId> made = pipeline(desc);
    if (!made.Ok()) {
        return ddraw4::fromError(made.Code());
    }
    platform::Result<std::uint32_t> first =
        stream(platform::BufferKind::kVertex, data, vertexCount * stride, stride);
    if (!first.Ok()) {
        return ddraw4::fromError(first.Code());
    }
    platform::DrawCall call;
    call.pipeline = made.Value();
    call.vertices = vertexStream_.buffer;
    call.firstVertex = first.Value();
    call.vertexCount = indices != nullptr ? vertexCount : drawn;
    if (indices != nullptr) {
        platform::Result<std::uint32_t> firstIndex =
            stream(platform::BufferKind::kIndex, indices, drawn * 2u, 2u);
        if (!firstIndex.Ok()) {
            return ddraw4::fromError(firstIndex.Code());
        }
        call.indices = indexStream_.buffer;
        call.firstIndex = firstIndex.Value();
        call.indexCount = drawn;
    }
    call.state = state;
    const platform::Status done = gpu().Draw(call);
    if (!done.Ok()) {
        return ddraw4::fromError(done.Code());
    }
    drawn_ = true;
    return kDdOk;
}

HResult Device::Clear(const Viewport &viewport, const std::vector<D3dRect> &rects,
                      std::uint32_t flags, std::uint32_t color, float z) {
    ddraw4::Surface *depth = target_->ZBuffer();
    if ((flags & kClearZBuffer) != 0 && depth == nullptr) {
        return kD3dErrZBufferNotPresent;
    }
    const ddraw4::RectL bounds{0, 0, static_cast<std::int32_t>(target_->Width()),
                               static_cast<std::int32_t>(target_->Height())};
    ddraw4::RectL limit = bounds;
    if (viewport.Desc()) {
        const ViewportDesc &desc = *viewport.Desc();
        limit = intersect(bounds, ddraw4::RectL{static_cast<std::int32_t>(desc.x),
                                                static_cast<std::int32_t>(desc.y),
                                                static_cast<std::int32_t>(desc.x + desc.width),
                                                static_cast<std::int32_t>(desc.y + desc.height)});
    }
    const float depthValue = std::clamp(z, 0.0f, 1.0f);
    // No rectangles at all means the whole viewport, as Direct3D clears with dwCount 0.
    const std::vector<D3dRect> whole{{limit.left, limit.top, limit.right, limit.bottom}};
    // Inside a scene the Gpu clears within its own current viewport, so it is set from the
    // clearing viewport first and put back to the device's current one afterwards.
    const bool switched = inScene_ && &viewport != current_;
    if (inScene_) {
        applyViewport(&viewport);
    }
    const HResult result = clearRects(rects.empty() ? whole : rects, limit, flags, color,
                                      depthValue, depth);
    if (switched) {
        applyViewport(current_);
    }
    return result;
}

HResult Device::clearRects(const std::vector<D3dRect> &rects, const ddraw4::RectL &limit,
                           std::uint32_t flags, std::uint32_t color, float depthValue,
                           ddraw4::Surface *depth) {
    for (const D3dRect &rect : rects) {
        const ddraw4::RectL area =
            intersect(limit, ddraw4::RectL{rect.x1, rect.y1, rect.x2, rect.y2});
        if (area.left >= area.right || area.top >= area.bottom) {
            continue;
        }
        if (inScene_) {
            platform::ClearDesc clear;
            clear.color = (flags & kClearTarget) != 0;
            clear.depth = (flags & kClearZBuffer) != 0;
            clear.colorValue = color;
            clear.depthValue = depthValue;
            clear.area = platform::Rect{area.left, area.top,
                                        static_cast<std::uint32_t>(area.right - area.left),
                                        static_cast<std::uint32_t>(area.bottom - area.top)};
            const platform::Status cleared = gpu().Clear(clear);
            if (!cleared.Ok()) {
                return ddraw4::fromError(cleared.Code());
            }
            drawn_ = true;
            continue;
        }
        // Outside a scene the Gpu fills the surfaces, as Blt with DDBLT_COLORFILL does.
        ddraw4::BltFx fx;
        if ((flags & kClearTarget) != 0) {
            fx.fillColor = ddraw4::packColor(target_->Format(), color);
            const HResult filled =
                target_->Blt(&area, nullptr, nullptr, ddraw4::kDdbltColorFill, &fx);
            if (filled != kDdOk) {
                return filled;
            }
        }
        if ((flags & kClearZBuffer) != 0) {
            fx.fillColor = static_cast<std::uint32_t>(depthValue * 65535.0f + 0.5f);
            const HResult filled =
                depth->Blt(&area, nullptr, nullptr, ddraw4::kDdbltColorFill, &fx);
            if (filled != kDdOk) {
                return filled;
            }
        }
    }
    return kDdOk;
}

} // namespace sfcop::hle::d3d3
