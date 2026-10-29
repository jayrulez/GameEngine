// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// VG core leaf types: vertex layout, gradient stop interpolation, fills, style.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.vg;

using namespace foundation::core;
using namespace foundation::vg;

TEST_CASE("vg.vertex: layout and solid helpers")
{
    CHECK(sizeof(VGVertex) == 44u);
    CHECK(VGVertex::SizeInBytes == 44);

    const VGVertex v = VGVertex::Solid(Float2{3.0f, 4.0f}, Color::Red);
    CHECK(v.position == Float2{3.0f, 4.0f});
    CHECK(v.texCoord == Float2{VGVertex::SolidUV, VGVertex::SolidUV});
    CHECK(v.color == Color::Red);
    CHECK(v.coverage == doctest::Approx(1.0f));
}

TEST_CASE("vg.vertex: the per-pipeline constructors write their values where the shaders read them")
{
    // The box shadow: the SDF operand rides texCoord and the signed corner radius coverage.
    const VGVertex shadow = VGVertex::BoxShadow(Float2{1.0f, 2.0f}, Float2{-0.5f, 0.25f}, Color::Red, -1.5f);
    CHECK(shadow.position == Float2{1.0f, 2.0f});
    CHECK(shadow.texCoord == Float2{-0.5f, 0.25f});
    CHECK(shadow.color == Color::Red);
    CHECK(shadow.coverage == -1.5f);
    CHECK(shadow.effect == Float2{0.0f, 0.0f});

    // A glyph: full coverage, the edge shift and ramp in effect.
    const VGVertex glyph = VGVertex::Glyph(Float2{3.0f, 4.0f}, Float2{0.1f, 0.2f}, Color::White, Float2{0.125f, 0.25f});
    CHECK(glyph.texCoord == Float2{0.1f, 0.2f});
    CHECK(glyph.coverage == 1.0f);
    CHECK(glyph.effect == Float2{0.125f, 0.25f});

    // A layer composite: the filter as offsets from none, so a plain composite carries zeros.
    const VGVertex plain = VGVertex::LayerComposite(Float2{0.0f, 0.0f}, Float2{1.0f, 1.0f}, Color::White, 1.0f, 1.0f);
    CHECK(plain.effect == Float2{0.0f, 0.0f});
    CHECK(plain.coverage == 1.0f);
    const VGVertex filtered = VGVertex::LayerComposite(Float2{0.0f, 0.0f}, Float2{1.0f, 1.0f}, Color::White, 0.25f, 1.5f);
    CHECK(filtered.effect == Float2{0.75f, 0.5f});
}

TEST_CASE("vg.fills: gradient stop interpolation")
{
    GradientStop stops[] = {GradientStop(0.0f, Color::Black), GradientStop(1.0f, Color::White)};
    const Span<const GradientStop> span(stops, 2);

    CHECK(NearlyEqual(ColorUtils::InterpolateStops(span, 0.0f), Color::Black));
    CHECK(NearlyEqual(ColorUtils::InterpolateStops(span, 1.0f), Color::White));
    CHECK(NearlyEqual(ColorUtils::InterpolateStops(span, 0.5f), Color{0.5f, 0.5f, 0.5f, 1.0f}));

    // Out-of-range clamps to the endpoints.
    CHECK(NearlyEqual(ColorUtils::InterpolateStops(span, -1.0f), Color::Black));
    CHECK(NearlyEqual(ColorUtils::InterpolateStops(span, 2.0f), Color::White));
}

TEST_CASE("vg.fills: solid and linear gradient")
{
    VGSolidFill solid(Color::Green);
    CHECK_FALSE(solid.RequiresInterpolation());
    CHECK(NearlyEqual(solid.GetColorAt(Float2{10.0f, 10.0f}, Rectangle{0, 0, 100, 100}),
                      Color::Green));

    VGLinearGradientFill grad(Float2{0.0f, 0.0f}, Float2{10.0f, 0.0f});
    grad.AddStop(0.0f, Color::Black);
    grad.AddStop(1.0f, Color::White);
    CHECK(grad.RequiresInterpolation());
    CHECK(NearlyEqual(grad.BaseColor(), Color::Black));
    CHECK(NearlyEqual(grad.GetColorAt(Float2{5.0f, 0.0f}, Rectangle{0, 0, 10, 10}),
                      Color{0.5f, 0.5f, 0.5f, 1.0f}));
}

TEST_CASE("vg.style: defaults")
{
    StrokeStyle s;
    CHECK(s.width == doctest::Approx(1.0f));
    CHECK(s.cap == VGLineCap::Butt);
    CHECK(s.join == VGLineJoin::Miter);
    CHECK(s.miterLimit == doctest::Approx(4.0f));

    CornerRadii r(5.0f);
    CHECK(r.IsUniform());
    CHECK_FALSE(r.IsZero());
}
