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

} // namespace sfcop::hle::d3d3
