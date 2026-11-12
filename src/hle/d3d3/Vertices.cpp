#include "hle/d3d3/Vertices.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sfcop::hle::d3d3 {

// Pass-through vertices are copied as the game's FVF lays them out (XYZRHW, diffuse, specular,
// texture sets), which is the Gpu layout's order. kFvfTlVertex is 16 + 4 + 4 + 8 bytes.
static_assert(platform::vertexStride(platform::VertexLayout{platform::PositionKind::kXyzRhw, false,
                                                            true, true, 1}) == 32,
              "the game's lit, transformed vertex is the Gpu's layout");

std::optional<VertexPlan> planVertices(std::uint32_t fvf) {
    VertexPlan plan;
    const std::uint32_t position = fvf & kFvfPositionMask;
    if (position != kFvfXyz && position != kFvfXyzRhw) {
        return std::nullopt; // blend weights are not drawn
    }
    plan.transformed = position == kFvfXyzRhw;
    plan.normal = (fvf & kFvfNormal) != 0;
    plan.reserved = (fvf & kFvfReserved1) != 0;
    plan.diffuse = (fvf & kFvfDiffuse) != 0;
    plan.specular = (fvf & kFvfSpecular) != 0;
    plan.texCoordSets = (fvf & kFvfTexCountMask) >> kFvfTexCountShift;
    if (plan.texCoordSets > 8) {
        return std::nullopt;
    }
    plan.sourceStride = (plan.transformed ? 16u : 12u) + (plan.normal ? 12u : 0u) +
                        (plan.reserved ? 4u : 0u) + (plan.diffuse ? 4u : 0u) +
                        (plan.specular ? 4u : 0u) + 8u * plan.texCoordSets;
    plan.passThrough = plan.transformed && !plan.normal && !plan.reserved && plan.texCoordSets <= 2;
    plan.layout.position = platform::PositionKind::kXyzRhw;
    plan.layout.normal = false;
    if (plan.passThrough) {
        plan.layout.diffuse = plan.diffuse;
        plan.layout.specular = plan.specular;
        plan.layout.texCoordSets = static_cast<std::uint8_t>(plan.texCoordSets);
    } else {
        plan.layout.diffuse = true;
        plan.layout.specular = true;
        plan.layout.texCoordSets = static_cast<std::uint8_t>(std::min(plan.texCoordSets, 2u));
    }
    return plan;
}

void transformPosition(const float in[3], const Matrix &m, const ViewportDesc &viewport,
                       float out[4]) {
    const float x = in[0] * m[0] + in[1] * m[4] + in[2] * m[8] + m[12];
    const float y = in[0] * m[1] + in[1] * m[5] + in[2] * m[9] + m[13];
    const float z = in[0] * m[2] + in[1] * m[6] + in[2] * m[10] + m[14];
    float w = in[0] * m[3] + in[1] * m[7] + in[2] * m[11] + m[15];
    if (w > -1e-6f && w < 1e-6f) {
        w = 1e-6f;
    }
    const float rhw = 1.0f / w;
    out[0] = static_cast<float>(viewport.x) +
             (x * rhw - viewport.clipX) / viewport.clipWidth * static_cast<float>(viewport.width);
    out[1] = static_cast<float>(viewport.y) +
             (viewport.clipY - y * rhw) / viewport.clipHeight * static_cast<float>(viewport.height);
    out[2] = viewport.minZ + z * rhw * (viewport.maxZ - viewport.minZ);
    out[3] = rhw;
}

std::vector<std::uint8_t> repackVertices(const VertexPlan &plan, const std::uint8_t *source,
                                         std::uint32_t count, const Matrix &transformMatrix,
                                         const ViewportDesc &viewport) {
    const std::uint32_t stride = platform::vertexStride(plan.layout);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(stride) * count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t *in = source + static_cast<std::size_t>(i) * plan.sourceStride;
        std::uint8_t *to = out.data() + static_cast<std::size_t>(i) * stride;
        float position[4];
        if (plan.transformed) {
            std::memcpy(position, in, 16);
            in += 16;
        } else {
            float xyz[3];
            std::memcpy(xyz, in, 12);
            in += 12;
            transformPosition(xyz, transformMatrix, viewport, position);
        }
        std::memcpy(to, position, 16);
        to += 16;
        in += (plan.normal ? 12u : 0u) + (plan.reserved ? 4u : 0u);
        std::uint32_t diffuse = 0xFFFFFFFF;  // unlit vertices are drawn white
        std::uint32_t specular = 0xFF000000; // no specular, no fog
        if (plan.diffuse) {
            std::memcpy(&diffuse, in, 4);
            in += 4;
        }
        if (plan.specular) {
            std::memcpy(&specular, in, 4);
            in += 4;
        }
        std::memcpy(to, &diffuse, 4);
        std::memcpy(to + 4, &specular, 4);
        std::memcpy(to + 8, in, 8u * plan.layout.texCoordSets);
    }
    return out;
}

std::vector<std::uint8_t> widenPrimitives(platform::PrimitiveType primitive,
                                          const std::uint8_t *data, std::uint32_t stride,
                                          const std::uint16_t *indices, std::uint32_t drawn) {
    std::vector<std::uint8_t> out;
    const auto vertex = [&](std::uint32_t i) {
        return data + static_cast<std::size_t>(indices != nullptr ? indices[i] : i) * stride;
    };
    const auto corner = [&](const std::uint8_t *from, float dx, float dy) {
        const std::size_t at = out.size();
        out.insert(out.end(), from, from + stride);
        float xy[2];
        std::memcpy(xy, from, sizeof xy);
        xy[0] += dx;
        xy[1] += dy;
        std::memcpy(out.data() + at, xy, sizeof xy);
    };
    const auto position = [](const std::uint8_t *from, float &x, float &y) {
        float xy[2];
        std::memcpy(xy, from, sizeof xy);
        x = xy[0];
        y = xy[1];
    };
    if (primitive == platform::PrimitiveType::kPointList) {
        out.reserve(static_cast<std::size_t>(drawn) * 6 * stride);
        for (std::uint32_t i = 0; i < drawn; ++i) {
            const std::uint8_t *p = vertex(i);
            corner(p, -0.5f, -0.5f);
            corner(p, 0.5f, -0.5f);
            corner(p, -0.5f, 0.5f);
            corner(p, 0.5f, -0.5f);
            corner(p, 0.5f, 0.5f);
            corner(p, -0.5f, 0.5f);
        }
        return out;
    }
    const bool strip = primitive == platform::PrimitiveType::kLineStrip;
    const std::uint32_t segments = strip ? drawn - 1 : drawn / 2;
    out.reserve(static_cast<std::size_t>(segments) * 6 * stride);
    for (std::uint32_t n = 0; n < segments; ++n) {
        const std::uint8_t *a = vertex(strip ? n : 2 * n);
        const std::uint8_t *b = vertex(strip ? n + 1 : 2 * n + 1);
        float ax = 0.0f;
        float ay = 0.0f;
        float bx = 0.0f;
        float by = 0.0f;
        position(a, ax, ay);
        position(b, bx, by);
        const float length = std::hypot(bx - ax, by - ay);
        if (!(length > 0.0f)) {
            continue;
        }
        // Half a logical pixel to each side of the line.
        const float nx = -(by - ay) / length * 0.5f;
        const float ny = (bx - ax) / length * 0.5f;
        corner(a, nx, ny);
        corner(a, -nx, -ny);
        corner(b, nx, ny);
        corner(a, -nx, -ny);
        corner(b, -nx, -ny);
        corner(b, nx, ny);
    }
    return out;
}

} // namespace sfcop::hle::d3d3
