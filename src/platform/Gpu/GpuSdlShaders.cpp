// The SDL GPU backend's shader programs: one vertex and one fragment program that together are
// Direct3D 3's fixed-function pipeline as Gpu.h describes it. Everything a pipeline does not fix
// in the SDL pipeline object (texture stage operations, colour keys, fog, alpha test, specular,
// flat shading) is a uniform, so one pair of programs serves every PipelineDesc.
//
// Formats: Metal Shading Language source, compiled by the driver when the device is created.
// SPIR-V (Vulkan) and DXIL (Direct3D 12) are byte arrays compiled offline from the same program;
// they land with the Windows and Linux builds (Wave 2), beside the MSL in shaderCode().
//
// The uniform blocks below are mirrored by VertexUniforms and FragmentUniforms in GpuSdl.cpp,
// member for member; both use only 16-byte vectors so that the layouts agree without padding.
#include <cstddef>
#include <cstdint>

namespace sfcop::platform::gpusdl {

namespace {

// Vertex attributes: 0 position (xyz, with rhw in w; Metal fills w with 1 for an xyz layout),
// 1 diffuse and 2 specular (bytes B, G, R, A, as 0xAARRGGBB lies in memory), 3 and 4 the two
// texture coordinate sets. An attribute the layout lacks is read from a constant buffer.
constexpr char kMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float4 position [[attribute(0)]];
    float4 diffuse [[attribute(1)]];
    float4 specular [[attribute(2)]];
    float2 tex0 [[attribute(3)]];
    float2 tex1 [[attribute(4)]];
};

struct VertexOut {
    float4 position [[position]];
    float pointSize [[point_size]];
    float4 diffuse;
    float4 diffuseFlat [[flat]];
    float4 specular;
    float4 specularFlat [[flat]];
    float2 uv0;
    float2 uv1;
    float depth;
};

// target: width, height, 1/width, 1/height of the colour target.
// select: x, y = the coordinate set stage 0 and stage 1 sample with.
struct VertexUniforms {
    float4 target;
    int4 select;
};

vertex VertexOut sfcop_vertex(VertexIn in [[stage_in]], constant VertexUniforms &u [[buffer(0)]]) {
    VertexOut out;
    // Direct3D 3 puts pixel centres on integer coordinates; Metal puts them at .5.
    float rhw = in.position.w > 0.0 ? in.position.w : 1.0;
    float w = 1.0 / rhw;
    float x = (in.position.x + 0.5) * u.target.z * 2.0 - 1.0;
    float y = 1.0 - (in.position.y + 0.5) * u.target.w * 2.0;
    out.position = float4(x * w, y * w, in.position.z * w, w);
    out.pointSize = 1.0;
    out.diffuse = in.diffuse.zyxw;
    out.diffuseFlat = out.diffuse;
    out.specular = in.specular.zyxw;
    out.specularFlat = out.specular;
    out.uv0 = u.select.x == 0 ? in.tex0 : in.tex1;
    out.uv1 = u.select.y == 0 ? in.tex0 : in.tex1;
    out.depth = in.position.z;
    return out;
}

// stage{0,1}a: colour op, colour arg 1, colour arg 2, alpha op.
// stage{0,1}b: alpha arg 1, alpha arg 2, colour key on (1/0), unused.
// key{0,1}: the colour key of each stage's texture, as normalised RGB.
// fog: start, end, density, mode (0 none, 1 vertex, 2 linear, 3 exp, 4 exp2).
// misc: alpha test function (CompareFunction; 7 = always), alpha reference 0-255,
//       specular add (1/0), flat shading (1/0).
struct FragmentUniforms {
    int4 stage0a;
    int4 stage0b;
    int4 stage1a;
    int4 stage1b;
    float4 key0;
    float4 key1;
    float4 textureFactor;
    float4 fogColor;
    float4 fog;
    int4 misc;
};

static float4 argument(int arg, float4 current, float4 diffuse, float4 specular, float4 tex,
                       float4 factor) {
    switch (arg) {
    case 0: return current;
    case 1: return diffuse;
    case 2: return specular;
    case 3: return tex;
    default: return factor;
    }
}

static float4 operation(int op, float4 a1, float4 a2, float4 diffuse, float4 tex, float4 factor,
                        float4 current) {
    switch (op) {
    case 1: return a1;
    case 2: return a2;
    case 3: return a1 * a2;
    case 4: return saturate(a1 * a2 * 2.0);
    case 5: return saturate(a1 * a2 * 4.0);
    case 6: return saturate(a1 + a2);
    case 7: return saturate(a1 + a2 - 0.5);
    case 8: return saturate(a1 - a2);
    case 9: return mix(a2, a1, diffuse.a);
    case 10: return mix(a2, a1, tex.a);
    case 11: return mix(a2, a1, factor.a);
    case 12: return mix(a2, a1, current.a);
    default: return current;
    }
}

static bool keyed(float4 texel, float4 key) {
    return all(abs(texel.rgb - key.rgb) < float3(0.5 / 255.0));
}

static bool passes(int function, float value, float reference) {
    switch (function) {
    case 0: return false;
    case 1: return value < reference;
    case 2: return value == reference;
    case 3: return value <= reference;
    case 4: return value > reference;
    case 5: return value != reference;
    case 6: return value >= reference;
    default: return true;
    }
}

static float4 stage(int4 a, int4 b, float4 key, float4 current, float4 diffuse, float4 specular,
                    float4 tex, float4 factor) {
    if (b.z != 0 && keyed(tex, key)) {
        discard_fragment();
    }
    float4 c1 = argument(a.y, current, diffuse, specular, tex, factor);
    float4 c2 = argument(a.z, current, diffuse, specular, tex, factor);
    float4 rgb = operation(a.x, c1, c2, diffuse, tex, factor, current);
    float alpha = current.a;
    if (a.w != 0) {
        float4 x1 = argument(b.x, current, diffuse, specular, tex, factor);
        float4 x2 = argument(b.y, current, diffuse, specular, tex, factor);
        alpha = operation(a.w, x1, x2, diffuse, tex, factor, current).a;
    }
    return float4(rgb.rgb, alpha);
}

fragment float4 sfcop_fragment(VertexOut in [[stage_in]],
                               constant FragmentUniforms &u [[buffer(0)]],
                               texture2d<float> texture0 [[texture(0)]],
                               sampler sampler0 [[sampler(0)]],
                               texture2d<float> texture1 [[texture(1)]],
                               sampler sampler1 [[sampler(1)]]) {
    float4 diffuse = u.misc.w != 0 ? in.diffuseFlat : in.diffuse;
    float4 specular = u.misc.w != 0 ? in.specularFlat : in.specular;
    float4 color = diffuse;
    if (u.stage0a.x != 0) {
        float4 tex = texture0.sample(sampler0, in.uv0);
        color = stage(u.stage0a, u.stage0b, u.key0, color, diffuse, specular, tex,
                      u.textureFactor);
        if (u.stage1a.x != 0) {
            float4 tex1 = texture1.sample(sampler1, in.uv1);
            color = stage(u.stage1a, u.stage1b, u.key1, color, diffuse, specular, tex1,
                          u.textureFactor);
        }
    }
    if (u.misc.z != 0) {
        color.rgb = saturate(color.rgb + specular.rgb);
    }
    int fogMode = int(u.fog.w);
    if (fogMode != 0) {
        float f = 1.0;
        if (fogMode == 1) {
            f = specular.a;
        } else if (fogMode == 2) {
            f = (u.fog.y - in.depth) / max(u.fog.y - u.fog.x, 1e-6);
        } else if (fogMode == 3) {
            f = exp(-u.fog.z * in.depth);
        } else {
            float d = u.fog.z * in.depth;
            f = exp(-d * d);
        }
        color.rgb = mix(u.fogColor.rgb, color.rgb, saturate(f));
    }
    if (!passes(u.misc.x, round(color.a * 255.0), float(u.misc.y))) {
        discard_fragment();
    }
    return color;
}
)MSL";

// SDL_GPU_SHADERFORMAT_MSL. The SPIR-V and DXIL bits join it in Wave 2.
constexpr std::uint32_t kFormatMsl = 1u << 4;

} // namespace

// The SDL_GPUShaderFormat bits this build carries code for.
std::uint32_t shaderFormats() { return kFormatMsl; }

// One program's code in `format`: false if this build has none.
bool shaderCode(std::uint32_t format, bool fragment, const std::uint8_t *&code, std::size_t &size,
                const char *&entry) {
    if (format != kFormatMsl) {
        return false;
    }
    code = reinterpret_cast<const std::uint8_t *>(kMsl);
    size = sizeof kMsl - 1;
    entry = fragment ? "sfcop_fragment" : "sfcop_vertex";
    return true;
}

} // namespace sfcop::platform::gpusdl
