// Ported from Sedulous.UI.Tests/src/PopupPositionerTests.bf (faithful). Beef `let (x,y) = ...` tuple ->
// Float2; RectangleF -> foundation::Rectangle; Vector2 -> Float2; Math.Abs(d) < eps -> doctest::Approx.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;

using namespace draconic::ui;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

TEST_CASE("popup-positioner: BestFit_PositionsBelowAnchor")
{
    const Float2 p = PopupPositioner::BestFit(Rectangle{100, 50, 80, 30}, Float2{120, 40},
                                              Rectangle{0, 0, 800, 600});
    CHECK(p.x == doctest::Approx(100));
    CHECK(p.y == doctest::Approx(80)); // below anchor
}

TEST_CASE("popup-positioner: BestFit_FlipsAboveWhenClippingBottom")
{
    const Float2 p = PopupPositioner::BestFit(Rectangle{100, 550, 80, 30}, Float2{120, 40},
                                              Rectangle{0, 0, 800, 600});
    CHECK(p.y < 550);                   // flipped above
    CHECK(p.y == doctest::Approx(510)); // 550 - 40
}

TEST_CASE("popup-positioner: BestFit_ClampsHorizontally")
{
    const Float2 p = PopupPositioner::BestFit(Rectangle{750, 50, 80, 30}, Float2{120, 40},
                                              Rectangle{0, 0, 800, 600});
    CHECK(p.x + 120 <= 800); // clamped to screen
}

TEST_CASE("popup-positioner: Center_CentersInScreen")
{
    const Float2 p = PopupPositioner::Center(Float2{200, 100}, Rectangle{0, 0, 800, 600});
    CHECK(p.x == doctest::Approx(300));
    CHECK(p.y == doctest::Approx(250));
}

TEST_CASE("popup-positioner: Below_PositionsDirectlyBelow")
{
    const Float2 p = PopupPositioner::Below(Rectangle{50, 100, 100, 30}, Float2{80, 40},
                                            Rectangle{0, 0, 800, 600});
    CHECK(p.x == doctest::Approx(50));
    CHECK(p.y == doctest::Approx(130));
}

TEST_CASE("popup-positioner: Above_PositionsDirectlyAbove")
{
    const Float2 p = PopupPositioner::Above(Rectangle{50, 100, 100, 30}, Float2{80, 40},
                                            Rectangle{0, 0, 800, 600});
    CHECK(p.x == doctest::Approx(50));
    CHECK(p.y == doctest::Approx(60)); // 100 - 40
}

TEST_CASE("popup-positioner: Submenu_PositionsToRight")
{
    const Float2 p = PopupPositioner::Submenu(Rectangle{100, 50, 150, 200}, Float2{120, 180},
                                              Rectangle{0, 0, 800, 600});
    CHECK(p.x == doctest::Approx(252)); // right edge of parent + 2px gap (clears the border)
    CHECK(p.y == doctest::Approx(50));
}

TEST_CASE("popup-positioner: Submenu_FlipsLeftWhenClipping")
{
    const Float2 p = PopupPositioner::Submenu(Rectangle{700, 50, 150, 200}, Float2{120, 180},
                                              Rectangle{0, 0, 800, 600});
    CHECK(p.x < 700); // flipped to left
}
