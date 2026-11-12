// How the game's vertices reach the Gpu (Device.h): the vertex formats it draws as they are, and
// the repacking, with the CPU transform of untransformed positions, for the others.
#pragma once

#include "hle/d3d3/D3d3Defs.h"

#include "platform/Gpu/Gpu.h"

#include <optional>
#include <vector>

namespace sfcop::hle::d3d3 {

// A vertex format the Gpu draws, and how the game's vertices of a format reach it.
struct VertexPlan {
    platform::VertexLayout layout;
    std::uint32_t sourceStride = 0;
    bool transformed = true; // XYZRHW
    bool normal = false;
    bool reserved = false;
    bool diffuse = false;
    bool specular = false;
    std::uint32_t texCoordSets = 0;
    bool passThrough = false;
};

// The plan for a flexible vertex format, or std::nullopt for one the module does not draw
// (blend weights, more than 8 coordinate sets). XYZRHW vertices with at most two coordinate sets
// and no normal or reserved field go through unchanged; others are repacked to XYZRHW, diffuse,
// specular and up to two sets.
std::optional<VertexPlan> planVertices(std::uint32_t fvf);

// The screen position, depth and 1/w of an untransformed position under `matrix` (world, view
// and projection) and the viewport's clip volume. A w of 0 is taken as a tiny positive w.
void transformPosition(const float in[3], const Matrix &matrix, const ViewportDesc &viewport,
                       float out[4]);

// `count` vertices of `plan`'s source format in its layout. Missing colours are white diffuse
// and black specular with no fog; normals are dropped (nothing is lit).
std::vector<std::uint8_t> repackVertices(const VertexPlan &plan, const std::uint8_t *source,
                                         std::uint32_t count, const Matrix &matrix,
                                         const ViewportDesc &viewport);

// Points and lines on a target with a render scale k, as triangles that cover what they cover at
// k = 1: a point is a square one logical pixel wide around it (k by k pixels of the target), a
// line a band one logical pixel wide along it (k pixels across). The Gpu draws a point or a line
// one pixel of its target wide, so without this the stars and lines of the 3D scene would shrink
// to a k-th of their width. `data` holds transformed vertices of `stride` bytes, x and y the
// first two floats; vertex i of the draw is `indices[i]` if indexed, else i. Every corner copies
// the vertex it stands for, so colours and texture coordinates stay as drawn. A line of zero
// length draws nothing, as on the Gpu.
std::vector<std::uint8_t> widenPrimitives(platform::PrimitiveType primitive,
                                          const std::uint8_t *data, std::uint32_t stride,
                                          const std::uint16_t *indices, std::uint32_t drawn);

} // namespace sfcop::hle::d3d3
