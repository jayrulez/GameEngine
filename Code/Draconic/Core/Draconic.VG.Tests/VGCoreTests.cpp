// VG core leaf types: vertex layout, gradient stop interpolation, fills, style.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.vg;

using namespace draconic::foundation;
using namespace draconic::vg;

TEST_CASE("vg.vertex: layout and solid helpers")
{
    CHECK(sizeof(VGVertex) == 36u);
    CHECK(VGVertex::SizeInBytes == 36);

    const VGVertex v = VGVertex::Solid(Float2{3.0f, 4.0f}, Color::Red);
    CHECK(v.position == Float2{3.0f, 4.0f});
    CHECK(v.texCoord == Float2{VGVertex::SolidUV, VGVertex::SolidUV});
    CHECK(v.color == Color::Red);
    CHECK(v.coverage == doctest::Approx(1.0f));
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
