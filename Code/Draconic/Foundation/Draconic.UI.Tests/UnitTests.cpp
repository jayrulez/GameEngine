// Ported from Sedulous.UI.Tests/src/UnitTests.bf (faithful; Math.Abs(x)<eps -> doctest::Approx).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;

using namespace draconic::ui;

TEST_CASE("unit: Dp_ResolveAtScale1") { CHECK(Unit::Dp(100.0f).Resolve(1.0f) == 100.0f); }
TEST_CASE("unit: Dp_ResolveAtScale2") { CHECK(Unit::Dp(100.0f).Resolve(2.0f) == 200.0f); }
TEST_CASE("unit: Dp_ResolveAtScale1_5")
{
    CHECK(Unit::Dp(100.0f).Resolve(1.5f) == doctest::Approx(150.0f));
}

TEST_CASE("unit: Px_IgnoresScale")
{
    Unit u = Unit::Px(50.0f);
    CHECK(u.Resolve(1.0f) == 50.0f);
    CHECK(u.Resolve(2.0f) == 50.0f);
    CHECK(u.Resolve(0.5f) == 50.0f);
}

TEST_CASE("unit: Pt_ResolveAtScale1")
{
    CHECK(Unit::Pt(14.0f).Resolve(1.0f) == doctest::Approx(14.0f * (96.0f / 72.0f)));
}
TEST_CASE("unit: Pt_ResolveAtScale2")
{
    CHECK(Unit::Pt(14.0f).Resolve(2.0f) == doctest::Approx(14.0f * 2.0f * (96.0f / 72.0f)));
}

TEST_CASE("unit: RawValue_ReturnsUnscaled")
{
    CHECK(Unit::Dp(42.0f).RawValue() == 42.0f);
    CHECK(Unit::Pt(14.0f).RawValue() == 14.0f);
    CHECK(Unit::Px(7.0f).RawValue() == 7.0f);
}
