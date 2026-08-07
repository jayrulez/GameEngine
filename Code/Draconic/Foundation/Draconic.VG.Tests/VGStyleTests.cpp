// Ported from Sedulous.VG.Tests/StyleTests.bf. Our IVGFill uses the engine's
// float Color (Sedulous used Color32), so fills take float Color inputs and we
// convert outputs via ToColor32 for the byte-channel assertions - keeping the
// Sedulous checks (R==0, R in (100,155), etc.) identical in spirit.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.vg;

using namespace draconic::foundation;
using namespace draconic::vg;

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
