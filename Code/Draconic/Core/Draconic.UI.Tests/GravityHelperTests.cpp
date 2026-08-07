// Ported from Sedulous.UI.Tests/src/GravityHelperTests.bf (faithful; RectangleF -> foundation::Rectangle,
// .X/.Y/.Width/.Height -> .x/.y/.width/.height).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;

using namespace draconic::ui;
namespace foundation = draconic::foundation;

TEST_CASE("gravity-helper: None_TopLeft")
{
    foundation::Rectangle r =
        GravityHelper::Apply(Gravity::None, 400.0f, 300.0f, 100.0f, 50.0f, Thickness{});
    CHECK(r.x == doctest::Approx(0.0f));
    CHECK(r.y == doctest::Approx(0.0f));
    CHECK(r.width == doctest::Approx(100.0f));
    CHECK(r.height == doctest::Approx(50.0f));
}

TEST_CASE("gravity-helper: Center")
{
    foundation::Rectangle r =
        GravityHelper::Apply(Gravity::Center, 400.0f, 300.0f, 100.0f, 50.0f, Thickness{});
    CHECK(r.x == doctest::Approx(150.0f));
    CHECK(r.y == doctest::Approx(125.0f));
}

TEST_CASE("gravity-helper: BottomRight")
{
    foundation::Rectangle r = GravityHelper::Apply(Gravity::Bottom | Gravity::Right, 400.0f, 300.0f,
                                             100.0f, 50.0f, Thickness{});
    CHECK(r.x == doctest::Approx(300.0f));
    CHECK(r.y == doctest::Approx(250.0f));
}

TEST_CASE("gravity-helper: Fill")
{
    foundation::Rectangle r =
        GravityHelper::Apply(Gravity::Fill, 400.0f, 300.0f, 100.0f, 50.0f, Thickness{});
    CHECK(r.x == doctest::Approx(0.0f));
    CHECK(r.y == doctest::Approx(0.0f));
    CHECK(r.width == doctest::Approx(400.0f));
    CHECK(r.height == doctest::Approx(300.0f));
}

TEST_CASE("gravity-helper: WithMargin")
{
    foundation::Rectangle r = GravityHelper::Apply(Gravity::Center, 400.0f, 300.0f, 100.0f, 50.0f,
                                             Thickness{10.0f, 20.0f, 10.0f, 20.0f});
    CHECK(r.x == doctest::Approx(150.0f)); // 10 + (380-100)/2
    CHECK(r.y == doctest::Approx(125.0f)); // 20 + (260-50)/2
}

TEST_CASE("gravity-helper: FillWithMargin")
{
    foundation::Rectangle r = GravityHelper::Apply(Gravity::Fill, 400.0f, 300.0f, 100.0f, 50.0f,
                                             Thickness{10.0f, 20.0f, 30.0f, 40.0f});
    CHECK(r.x == doctest::Approx(10.0f));
    CHECK(r.y == doctest::Approx(20.0f));
    CHECK(r.width == doctest::Approx(360.0f));  // 400-10-30
    CHECK(r.height == doctest::Approx(240.0f)); // 300-20-40
}
