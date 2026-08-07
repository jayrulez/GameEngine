// Ported from Sedulous.UI.Tests/src/SizeSpecTests.bf (faithful; SizeSpec.Match/.Wrap -> Match()/Wrap()).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;

using namespace draconic::ui;

TEST_CASE("size-spec: Fixed_CarriesUnit")
{
    SizeSpec spec = SizeSpec::Fixed(Unit::Dp(120.0f));
    CHECK(spec.IsFixed());
    CHECK(spec.ResolveFixed(1.0f) == doctest::Approx(120.0f));
    CHECK(spec.ResolveFixed(2.0f) == doctest::Approx(240.0f));
}

TEST_CASE("size-spec: Fixed_WithPx")
{
    SizeSpec spec = SizeSpec::Fixed(Unit::Px(50.0f));
    CHECK(spec.IsFixed());
    CHECK(spec.ResolveFixed(1.0f) == 50.0f);
    CHECK(spec.ResolveFixed(2.0f) == 50.0f); // Px ignores scale
}

TEST_CASE("size-spec: Match_IsNotFixed")
{
    SizeSpec spec = SizeSpec::Match();
    CHECK_FALSE(spec.IsFixed());
    CHECK(spec.ResolveFixed(1.0f) == 0.0f);
}

TEST_CASE("size-spec: Wrap_IsNotFixed")
{
    SizeSpec spec = SizeSpec::Wrap();
    CHECK_FALSE(spec.IsFixed());
    CHECK(spec.ResolveFixed(1.0f) == 0.0f);
}
