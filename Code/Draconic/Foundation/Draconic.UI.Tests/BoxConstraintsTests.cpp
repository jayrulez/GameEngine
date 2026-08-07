// Ported from Sedulous.UI.Tests/src/BoxConstraintsTests.bf (faithful; float.MaxValue -> kFloatMax).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;

using namespace draconic::ui;
namespace foundation = draconic::foundation;

TEST_CASE("box-constraints: Tight_SetsMinEqualMax")
{
    BoxConstraints c = BoxConstraints::Tight(100.0f, 50.0f);
    CHECK(c.MinWidth == 100.0f);
    CHECK(c.MaxWidth == 100.0f);
    CHECK(c.MinHeight == 50.0f);
    CHECK(c.MaxHeight == 50.0f);
    CHECK(c.IsTight());
}

TEST_CASE("box-constraints: Loose_SetsZeroMin")
{
    BoxConstraints c = BoxConstraints::Loose(200.0f, 100.0f);
    CHECK(c.MinWidth == 0.0f);
    CHECK(c.MaxWidth == 200.0f);
    CHECK(c.MinHeight == 0.0f);
    CHECK(c.MaxHeight == 100.0f);
    CHECK(c.IsLoose());
    CHECK_FALSE(c.IsTight());
}

TEST_CASE("box-constraints: Expand_IsUnconstrained")
{
    BoxConstraints c = BoxConstraints::Expand();
    CHECK(c.MinWidth == 0.0f);
    CHECK(c.MaxWidth == foundation::kFloatMax);
    CHECK(c.MinHeight == 0.0f);
    CHECK(c.MaxHeight == foundation::kFloatMax);
}

TEST_CASE("box-constraints: Deflate_ShrinksByPadding")
{
    BoxConstraints c = BoxConstraints::Tight(200.0f, 100.0f);
    BoxConstraints d = c.Deflate(Thickness{10.0f, 5.0f, 10.0f, 5.0f});
    CHECK(d.MinWidth == doctest::Approx(180.0f));
    CHECK(d.MaxWidth == doctest::Approx(180.0f));
    CHECK(d.MinHeight == doctest::Approx(90.0f));
    CHECK(d.MaxHeight == doctest::Approx(90.0f));
}

TEST_CASE("box-constraints: Deflate_ClampsToZero")
{
    BoxConstraints c = BoxConstraints::Tight(10.0f, 10.0f);
    BoxConstraints d = c.Deflate(Thickness{20.0f, 20.0f, 20.0f, 20.0f});
    CHECK(d.MinWidth == 0.0f);
    CHECK(d.MaxWidth == 0.0f);
    CHECK(d.MinHeight == 0.0f);
    CHECK(d.MaxHeight == 0.0f);
}

TEST_CASE("box-constraints: ConstrainWidth_ClampsToRange")
{
    BoxConstraints c{50.0f, 200.0f, 0.0f, 100.0f};
    CHECK(c.ConstrainWidth(30.0f) == 50.0f);   // below min
    CHECK(c.ConstrainWidth(100.0f) == 100.0f); // within range
    CHECK(c.ConstrainWidth(300.0f) == 200.0f); // above max
}

TEST_CASE("box-constraints: ConstrainHeight_ClampsToRange")
{
    BoxConstraints c{0.0f, 100.0f, 25.0f, 75.0f};
    CHECK(c.ConstrainHeight(10.0f) == 25.0f);
    CHECK(c.ConstrainHeight(50.0f) == 50.0f);
    CHECK(c.ConstrainHeight(100.0f) == 75.0f);
}

TEST_CASE("box-constraints: Loosen_KeepsMaxZeroesMin")
{
    BoxConstraints c{50.0f, 200.0f, 30.0f, 100.0f};
    BoxConstraints l = c.Loosen();
    CHECK(l.MinWidth == 0.0f);
    CHECK(l.MaxWidth == 200.0f);
    CHECK(l.MinHeight == 0.0f);
    CHECK(l.MaxHeight == 100.0f);
}

TEST_CASE("box-constraints: TightenToMax_SetsMinToMax")
{
    BoxConstraints c = BoxConstraints::Loose(300.0f, 150.0f);
    BoxConstraints t = c.TightenToMax();
    CHECK(t.MinWidth == 300.0f);
    CHECK(t.MaxWidth == 300.0f);
    CHECK(t.MinHeight == 150.0f);
    CHECK(t.MaxHeight == 150.0f);
    CHECK(t.IsTight());
}
