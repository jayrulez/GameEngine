// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Ported from Sedulous.UI.Tests/src/SizeSpecTests.bf (faithful; SizeSpec.Match/.Wrap -> Match()/Wrap()).
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;

using namespace foundation::ui;

TEST_CASE("size-spec: Fixed_CarriesUnit")
{
    SizeSpec spec = SizeSpec::Fixed(Unit::Dp(120.0f));
    CHECK(spec.IsFixed());
    CHECK(spec.ResolveFixed(1.0f) == doctest::Approx(120.0f));
    CHECK(spec.ResolveFixed(2.0f) == doctest::Approx(120.0f)); // Dp is logical (P2b)
}

TEST_CASE("size-spec: Fixed_WithPx")
{
    SizeSpec spec = SizeSpec::Fixed(Unit::Px(50.0f));
    CHECK(spec.IsFixed());
    CHECK(spec.ResolveFixed(1.0f) == 50.0f);
    CHECK(spec.ResolveFixed(2.0f) == 25.0f); // Px = PHYSICAL pixels -> 25 logical at 2x (P2b)
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
