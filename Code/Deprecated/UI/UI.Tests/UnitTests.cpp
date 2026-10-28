// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Ported from Sedulous.UI.Tests/src/UnitTests.bf, for LOGICAL unit semantics:
// layout runs in logical space and the root applies DpiScale once at draw.
// Dp is identity at resolve (a value*scale here would DOUBLE-scale), Px divides by the scale so it
// lands on exact device pixels, Pt is 96/72 logical.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;

using namespace foundation::ui;

TEST_CASE("unit: Dp_ResolveAtScale1") { CHECK(Unit::Dp(100.0f).Resolve(1.0f) == 100.0f); }
TEST_CASE("unit: Dp_IsLogicalIdentityAtAnyScale")
{
    CHECK(Unit::Dp(100.0f).Resolve(2.0f) == 100.0f);
    CHECK(Unit::Dp(100.0f).Resolve(1.5f) == doctest::Approx(100.0f));
}

TEST_CASE("unit: Px_MeansPhysicalPixels")
{
    // 50 physical px = 25 logical at 2x (draw scales x2 back to exactly 50 device px).
    Unit u = Unit::Px(50.0f);
    CHECK(u.Resolve(1.0f) == 50.0f);
    CHECK(u.Resolve(2.0f) == 25.0f);
    CHECK(u.Resolve(0.5f) == 100.0f);
}

TEST_CASE("unit: Pt_ResolveAtScale1")
{
    CHECK(Unit::Pt(14.0f).Resolve(1.0f) == doctest::Approx(14.0f * (96.0f / 72.0f)));
}
TEST_CASE("unit: Pt_IsLogicalAtAnyScale")
{
    CHECK(Unit::Pt(14.0f).Resolve(2.0f) == doctest::Approx(14.0f * (96.0f / 72.0f)));
}

TEST_CASE("unit: RawValue_ReturnsUnscaled")
{
    CHECK(Unit::Dp(42.0f).RawValue() == 42.0f);
    CHECK(Unit::Pt(14.0f).RawValue() == 14.0f);
    CHECK(Unit::Px(7.0f).RawValue() == 7.0f);
}
