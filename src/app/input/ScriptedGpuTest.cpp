#include "app/input/ScriptedGpu.h"
#include "platform/Clock/ClockMock.h"
#include "platform/Gpu/GpuMock.h"
#include "platform/Window/WindowMock.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using sfcop::app::input::parseInputScript;
using sfcop::app::input::ScriptedGpu;
using sfcop::app::input::ScriptedWindow;
using sfcop::app::input::ScriptPlayer;

TEST_CASE("the Gpu given the scripted window attaches to the window it wraps") {
    sfcop::platform::WindowMock inner;
    sfcop::platform::WindowMock other;
    sfcop::platform::ClockMock clock;
    sfcop::platform::GpuMock gpuInner;
    ScriptPlayer player(parseInputScript("").script, {});
    ScriptedWindow window(inner, player, clock, {});
    ScriptedGpu gpu(gpuInner, window);

    CHECK(&gpu.AttachTarget(window) == &inner);
    CHECK(&gpu.AttachTarget(other) == &other);
    CHECK(gpu.Attach(window).Ok());
    CHECK(gpu.Present().Ok());
    CHECK(gpu.Detach().Ok());
    CHECK(gpuInner.log.Count("Attach") == 1);
    CHECK(gpuInner.log.Count("Present") == 1);
    CHECK(gpuInner.log.Count("Detach") == 1);
}

TEST_CASE("the scripted Gpu hands the present filter to the Gpu it wraps, with its answer") {
    sfcop::platform::WindowMock inner;
    sfcop::platform::ClockMock clock;
    sfcop::platform::GpuMock gpuInner;
    ScriptPlayer player(parseInputScript("").script, {});
    ScriptedWindow window(inner, player, clock, {});
    ScriptedGpu gpu(gpuInner, window);

    CHECK(gpu.SetPresentFilter(sfcop::platform::TextureFilter::kLinear).Ok());
    CHECK(gpuInner.log.Calls() == std::vector<std::string>{"SetPresentFilter(1)"});
    gpuInner.failWith = sfcop::platform::Error::kUnsupported;
    CHECK(gpu.SetPresentFilter(sfcop::platform::TextureFilter::kPoint).Code() ==
          sfcop::platform::Error::kUnsupported);
    CHECK(gpuInner.log.Count("SetPresentFilter") == 2);
}

TEST_CASE("the scripted Gpu hands a render-scaled back buffer to the Gpu it wraps") {
    sfcop::platform::WindowMock inner;
    sfcop::platform::ClockMock clock;
    sfcop::platform::GpuMock gpuInner;
    ScriptPlayer player(parseInputScript("").script, {});
    ScriptedWindow window(inner, player, clock, {});
    ScriptedGpu gpu(gpuInner, window);

    const auto back =
        gpu.ConfigureScaledBackBuffer(800, 600, sfcop::platform::PixelFormat::kRgb565, 3);
    REQUIRE(back.Ok());
    CHECK(gpuInner.log.Count("ConfigureScaledBackBuffer") == 1);
    CHECK(gpuInner.textureScales.at(back.Value().value) == 3);
}
