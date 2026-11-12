#include "hle/d3d3/Vertices.h"

#include <doctest/doctest.h>

#include <cstring>

namespace sfcop::hle::d3d3 {

namespace {

ViewportDesc screen800x600() {
    ViewportDesc viewport;
    viewport.width = 800;
    viewport.height = 600;
    return viewport;
}

} // namespace

TEST_CASE("the game's transformed vertex goes to the Gpu unchanged") {
    const auto plan = planVertices(kFvfTlVertex);
    REQUIRE(plan.has_value());
    CHECK(plan->passThrough);
    CHECK(plan->sourceStride == 32u);
    CHECK(plan->layout.position == platform::PositionKind::kXyzRhw);
    CHECK(plan->layout.diffuse);
    CHECK(plan->layout.specular);
    CHECK(plan->layout.texCoordSets == 1);
}

TEST_CASE("formats the Gpu cannot draw as they are get a repacking plan or none") {
    const auto withNormal = planVertices(kFvfXyz | kFvfNormal | kFvfDiffuse | 0x100);
    REQUIRE(withNormal.has_value());
    CHECK_FALSE(withNormal->passThrough);
    CHECK_FALSE(withNormal->transformed);
    CHECK(withNormal->sourceStride == 36u); // xyz, normal, diffuse, one set
    CHECK(withNormal->layout.diffuse);
    CHECK(withNormal->layout.specular);
    CHECK(withNormal->layout.texCoordSets == 1);
    CHECK_FALSE(planVertices(0x006).has_value()); // XYZB1
}

TEST_CASE("an untransformed position maps through the clip volume onto the viewport") {
    const ViewportDesc viewport = screen800x600();
    const float centre[3] = {0.0f, 0.0f, 0.5f};
    float out[4] = {};
    transformPosition(centre, kIdentity, viewport, out);
    CHECK(out[0] == 400.0f);
    CHECK(out[1] == 300.0f);
    CHECK(out[2] == 0.5f);
    CHECK(out[3] == 1.0f);

    const float corner[3] = {1.0f, -1.0f, 0.0f};
    transformPosition(corner, kIdentity, viewport, out);
    CHECK(out[0] == 800.0f);
    CHECK(out[1] == 600.0f);

    const Matrix halfW{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 2}; // w = 2
    const float right[3] = {1.0f, 0.0f, 0.0f};
    transformPosition(right, halfW, viewport, out);
    CHECK(out[0] == 600.0f);
    CHECK(out[3] == 0.5f);
}

TEST_CASE("repacking fills in white diffuse and black specular") {
    const auto plan = planVertices(kFvfXyz);
    REQUIRE(plan.has_value());
    const float source[3] = {0.0f, 0.0f, 0.25f};
    const std::vector<std::uint8_t> out = repackVertices(
        *plan, reinterpret_cast<const std::uint8_t *>(source), 1, kIdentity, screen800x600());
    REQUIRE(out.size() == 24u);
    float position[4];
    std::uint32_t colors[2];
    std::memcpy(position, out.data(), 16);
    std::memcpy(colors, out.data() + 16, 8);
    CHECK(position[0] == 400.0f);
    CHECK(position[1] == 300.0f);
    CHECK(position[2] == 0.25f);
    CHECK(position[3] == 1.0f);
    CHECK(colors[0] == 0xFFFFFFFFu);
    CHECK(colors[1] == 0xFF000000u);
}

namespace {

// A transformed vertex as the game draws it (kFvfTlVertex, 32 bytes).
struct Tl {
    float x, y, z, rhw;
    std::uint32_t diffuse, specular;
    float u, v;
};

std::vector<Tl> unpack(const std::vector<std::uint8_t> &bytes) {
    std::vector<Tl> out(bytes.size() / sizeof(Tl));
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return out;
}

const std::uint8_t *raw(const Tl *vertices) {
    return reinterpret_cast<const std::uint8_t *>(vertices);
}

} // namespace

TEST_CASE("a point widens to a square one logical pixel across, its vertex copied") {
    const Tl point[1] = {{10.0f, 20.0f, 0.25f, 0.5f, 0xFF112233, 0x80000000, 0.5f, 0.75f}};
    const std::vector<Tl> out = unpack(
        widenPrimitives(platform::PrimitiveType::kPointList, raw(point), sizeof(Tl), nullptr, 1));
    REQUIRE(out.size() == 6);
    // Two triangles over x 9.5-10.5, y 19.5-20.5: the k by k block of the point's pixel.
    const float corners[6][2] = {{9.5f, 19.5f},  {10.5f, 19.5f}, {9.5f, 20.5f},
                                 {10.5f, 19.5f}, {10.5f, 20.5f}, {9.5f, 20.5f}};
    for (std::size_t i = 0; i < 6; ++i) {
        CHECK(out[i].x == corners[i][0]);
        CHECK(out[i].y == corners[i][1]);
        CHECK(out[i].z == 0.25f);
        CHECK(out[i].rhw == 0.5f);
        CHECK(out[i].diffuse == 0xFF112233);
        CHECK(out[i].specular == 0x80000000);
        CHECK(out[i].u == 0.5f);
        CHECK(out[i].v == 0.75f);
    }
}

TEST_CASE("indexed points widen in index order") {
    const Tl points[2] = {{1.0f, 1.0f, 0, 1, 0xFF0000FF, 0, 0, 0},
                          {5.0f, 7.0f, 0, 1, 0xFFFF0000, 0, 0, 0}};
    const std::uint16_t indices[1] = {1};
    const std::vector<Tl> out = unpack(
        widenPrimitives(platform::PrimitiveType::kPointList, raw(points), sizeof(Tl), indices, 1));
    REQUIRE(out.size() == 6);
    CHECK(out[0].x == 4.5f);
    CHECK(out[0].y == 6.5f);
    CHECK(out[0].diffuse == 0xFFFF0000);
}

TEST_CASE("a line widens to a band half a logical pixel to each side, each end its own colour") {
    const Tl line[2] = {{0.0f, 4.0f, 0, 1, 0xFF0000FF, 0, 0, 0},
                        {8.0f, 4.0f, 0, 1, 0xFFFF0000, 0, 0, 0}};
    const std::vector<Tl> out = unpack(
        widenPrimitives(platform::PrimitiveType::kLineList, raw(line), sizeof(Tl), nullptr, 2));
    REQUIRE(out.size() == 6);
    // Along x: the band is y 3.5-4.5 from x 0 to x 8.
    const float corners[6][2] = {{0.0f, 4.5f}, {0.0f, 3.5f}, {8.0f, 4.5f},
                                 {0.0f, 3.5f}, {8.0f, 3.5f}, {8.0f, 4.5f}};
    const std::uint32_t colours[6] = {0xFF0000FF, 0xFF0000FF, 0xFFFF0000,
                                      0xFF0000FF, 0xFFFF0000, 0xFFFF0000};
    for (std::size_t i = 0; i < 6; ++i) {
        CHECK(out[i].x == corners[i][0]);
        CHECK(out[i].y == corners[i][1]);
        CHECK(out[i].diffuse == colours[i]);
    }
}

TEST_CASE("a line strip widens segment by segment and a zero-length segment draws nothing") {
    const Tl strip[3] = {{0.0f, 0.0f, 0, 1, 0, 0, 0, 0},
                         {0.0f, 0.0f, 0, 1, 0, 0, 0, 0},
                         {0.0f, 6.0f, 0, 1, 0, 0, 0, 0}};
    const std::vector<Tl> out = unpack(
        widenPrimitives(platform::PrimitiveType::kLineStrip, raw(strip), sizeof(Tl), nullptr, 3));
    REQUIRE(out.size() == 6); // the second segment only
    // Along y: the band is x -0.5 to 0.5.
    CHECK(out[0].x == -0.5f);
    CHECK(out[0].y == 0.0f);
    CHECK(out[1].x == 0.5f);
    CHECK(out[2].x == -0.5f);
    CHECK(out[2].y == 6.0f);
}

} // namespace sfcop::hle::d3d3
