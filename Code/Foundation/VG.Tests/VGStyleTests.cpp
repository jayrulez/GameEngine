// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Fill styles: solid and gradient fills. IVGFill works in float Color, so outputs go through
// ToColor32 where an assertion checks byte channels.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <initializer_list>
import foundation.core;
import foundation.vg;

using namespace foundation::core;
using namespace foundation::vg;

TEST_CASE("style: solid fill returns same color")
{
    VGSolidFill fill(Color::Red);
    const Color32 c1 = ToColor32(fill.GetColorAt(Float2{0, 0}, Rectangle{0, 0, 100, 100}));
    const Color32 c2 = ToColor32(fill.GetColorAt(Float2{50, 50}, Rectangle{0, 0, 100, 100}));
    CHECK(c1 == Color32::Red);
    CHECK(c2 == Color32::Red);
    CHECK_FALSE(fill.RequiresInterpolation());
}

TEST_CASE("style: linear gradient two-stop interpolates")
{
    VGLinearGradientFill fill(Float2{0, 0}, Float2{100, 0});
    fill.AddStop(0, Color::Black);
    fill.AddStop(1, Color::White);

    const Color32 atStart = ToColor32(fill.GetColorAt(Float2{0, 0}, Rectangle{0, 0, 100, 100}));
    const Color32 atEnd = ToColor32(fill.GetColorAt(Float2{100, 0}, Rectangle{0, 0, 100, 100}));
    const Color32 atMid = ToColor32(fill.GetColorAt(Float2{50, 0}, Rectangle{0, 0, 100, 100}));

    CHECK(atStart.r == 0);
    CHECK(atEnd.r == 255);
    CHECK((atMid.r > 100 && atMid.r < 155));
}

TEST_CASE("style: linear gradient multi-stop")
{
    VGLinearGradientFill fill(Float2{0, 0}, Float2{100, 0});
    fill.AddStop(0, Color::Red);
    fill.AddStop(0.5f, Color::Green);
    fill.AddStop(1, Color::Blue);

    const Color32 atStart = ToColor32(fill.GetColorAt(Float2{0, 0}, Rectangle{}));
    const Color32 atMid = ToColor32(fill.GetColorAt(Float2{50, 0}, Rectangle{}));
    const Color32 atEnd = ToColor32(fill.GetColorAt(Float2{100, 0}, Rectangle{}));

    CHECK((atStart.r == 255 && atStart.g == 0));
    CHECK(atMid.g > 100);
    CHECK((atEnd.b == 255 && atEnd.r == 0));
}

TEST_CASE("style: radial gradient center and edge")
{
    VGRadialGradientFill fill(Float2{50, 50}, 50);
    fill.AddStop(0, Color::White);
    fill.AddStop(1, Color::Black);

    const Color32 atCenter = ToColor32(fill.GetColorAt(Float2{50, 50}, Rectangle{}));
    const Color32 atEdge = ToColor32(fill.GetColorAt(Float2{100, 50}, Rectangle{}));
    CHECK(atCenter.r == 255);
    CHECK(atEdge.r == 0);
}

TEST_CASE("style: conic gradient at angles")
{
    VGConicGradientFill fill(Float2{50, 50}, 0);
    fill.AddStop(0, Color::Red);
    fill.AddStop(1, Color::Blue);

    const Color32 atRight = ToColor32(fill.GetColorAt(Float2{100, 50}, Rectangle{}));
    CHECK(atRight.r > 200);
    const Color32 atLeft = ToColor32(fill.GetColorAt(Float2{0, 50}, Rectangle{}));
    CHECK((atLeft.r > 0 || atLeft.b > 0));
}

TEST_CASE("style: ColorUtils lerp color")
{
    const Color32 result = ToColor32(ColorUtils::LerpColor(Color::Black, Color::White, 0.5f));
    CHECK((result.r > 120 && result.r < 135));
    CHECK((result.g > 120 && result.g < 135));
    CHECK((result.b > 120 && result.b < 135));
}

TEST_CASE("style: ColorUtils interpolate stops boundary clamp")
{
    GradientStop stops[2] = {GradientStop(0.2f, Color::Red), GradientStop(0.8f, Color::Blue)};
    const Color32 before =
        ToColor32(ColorUtils::InterpolateStops(Span<const GradientStop>(stops, 2), 0.0f));
    const Color32 after =
        ToColor32(ColorUtils::InterpolateStops(Span<const GradientStop>(stops, 2), 1.0f));
    CHECK(before == Color32::Red);
    CHECK(after == Color32::Blue);
}

TEST_CASE("style: the coordinate handed to the shader agrees with the CPU parameter")
{
    // The GPU gradient path hands the shader fill.GradientCoord per vertex and derives t per
    // pixel: radial takes length(coord), conic takes frac(atan2(coord.y, coord.x) / 2pi)
    // (vg_grad_radial.ps.hlsl and vg_grad_conic.ps.hlsl). GetParameterAt is the CPU truth. If
    // either side drifts the GPU draws something the CPU never described, and nothing else
    // catches it, so the shader math is replayed here at several points.
    const Rectangle bounds{0.0f, 0.0f, 100.0f, 100.0f};
    const Float2 points[] = {Float2{60.0f, 50.0f}, Float2{50.0f, 20.0f}, Float2{35.0f, 35.0f},
                             Float2{80.0f, 90.0f}, Float2{10.0f, 55.0f}};

    VGRadialGradientFill radial(Float2{50.0f, 50.0f}, 25.0f);
    for (const Float2 p : points)
    {
        const Float2 coord = radial.GradientCoord(p, bounds);
        CHECK(Length(coord) == doctest::Approx(radial.GetParameterAt(p, bounds)).epsilon(1e-4));
    }

    for (const f32 startAngle : {0.0f, 0.7f, kPi, -2.0f})
    {
        VGConicGradientFill conic(Float2{50.0f, 50.0f}, startAngle);
        for (const Float2 p : points)
        {
            const Float2 coord = conic.GradientCoord(p, bounds);
            f32 a = Atan2(coord.y, coord.x) / kTwoPi; // (-0.5, 0.5], what the shader computes
            a -= Floor(a);                             // frac into [0, 1)
            const f32 cpu = conic.GetParameterAt(p, bounds);
            // Both live on a circle: compare modulo 1 so a value at the wrap (0 vs 1) matches.
            f32 diff = Abs(a - cpu);
            diff = diff > 0.5f ? 1.0f - diff : diff;
            CHECK(diff < 1e-4f);
        }
    }
}
