// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Ported from Sedulous.UI.Tests/src/GravityTests.bf (faithful; enum-vs-int compares via underlying u32).
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;

using namespace foundation::ui;
namespace core = foundation::core;

TEST_CASE("gravity: None_IsZero") { CHECK(static_cast<core::u32>(Gravity::None) == 0u); }

TEST_CASE("gravity: Center_IsCenterHOrCenterV")
{
    CHECK(Gravity::Center == (Gravity::CenterH | Gravity::CenterV));
}

TEST_CASE("gravity: Fill_IsFillHOrFillV")
{
    CHECK(Gravity::Fill == (Gravity::FillH | Gravity::FillV));
}

TEST_CASE("gravity: Combinations_Work")
{
    CHECK((Gravity::Top | Gravity::Right) == Gravity::TopRight);
    CHECK((Gravity::Bottom | Gravity::Left) == Gravity::BottomLeft);
}

TEST_CASE("gravity: FlagsAreDistinct")
{
    CHECK(static_cast<core::u32>(Gravity::Left & Gravity::Right) == 0u);
    CHECK(static_cast<core::u32>(Gravity::Top & Gravity::Bottom) == 0u);
    CHECK(static_cast<core::u32>(Gravity::CenterH & Gravity::FillH) == 0u);
    CHECK(static_cast<core::u32>(Gravity::CenterV & Gravity::FillV) == 0u);
    CHECK(static_cast<core::u32>(Gravity::Left & Gravity::Top) == 0u);
}
