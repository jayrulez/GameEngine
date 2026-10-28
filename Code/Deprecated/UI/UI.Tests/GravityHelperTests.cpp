// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Ported from Sedulous.UI.Tests/src/GravityHelperTests.bf, for the margin-box
// contract: Apply positions the MARGIN BOX (no margin parameter); View::Layout
// insets to the border box. The margin cases below assert the margin-box rect and note the final
// border box.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;

using namespace foundation::ui;
namespace core = foundation::core;

TEST_CASE("gravity-helper: None_TopLeft")
{
    core::Rectangle r = GravityHelper::Apply(Gravity::None, 400.0f, 300.0f, 100.0f, 50.0f);
    CHECK(r.x == doctest::Approx(0.0f));
    CHECK(r.y == doctest::Approx(0.0f));
    CHECK(r.width == doctest::Approx(100.0f));
    CHECK(r.height == doctest::Approx(50.0f));
}

TEST_CASE("gravity-helper: Center")
{
    core::Rectangle r = GravityHelper::Apply(Gravity::Center, 400.0f, 300.0f, 100.0f, 50.0f);
    CHECK(r.x == doctest::Approx(150.0f));
    CHECK(r.y == doctest::Approx(125.0f));
}

TEST_CASE("gravity-helper: BottomRight")
{
    core::Rectangle r =
        GravityHelper::Apply(Gravity::Bottom | Gravity::Right, 400.0f, 300.0f, 100.0f, 50.0f);
    CHECK(r.x == doctest::Approx(300.0f));
    CHECK(r.y == doctest::Approx(250.0f));
}

TEST_CASE("gravity-helper: Fill")
{
    core::Rectangle r = GravityHelper::Apply(Gravity::Fill, 400.0f, 300.0f, 100.0f, 50.0f);
    CHECK(r.x == doctest::Approx(0.0f));
    CHECK(r.y == doctest::Approx(0.0f));
    CHECK(r.width == doctest::Approx(400.0f));
    CHECK(r.height == doctest::Approx(300.0f));
}

TEST_CASE("gravity-helper: WithMargin_MarginBoxCentered")
{
    // Child 100x50 with margins {10,20,10,20} = margin box 120x90, centered in 400x300.
    core::Rectangle r = GravityHelper::Apply(Gravity::Center, 400.0f, 300.0f, 120.0f, 90.0f);
    CHECK(r.x == doctest::Approx(140.0f)); // (400-120)/2
    CHECK(r.y == doctest::Approx(105.0f)); // (300-90)/2
    // View::Layout insets by margin -> border box at (150,125) 100x50: identical to pre-P2b.
}

TEST_CASE("gravity-helper: FillWithMargin_MarginBoxFills")
{
    // Fill hands the whole container to the MARGIN box; View::Layout insets by
    // {10,20,30,40} -> border box (10,20) 360x240: identical to pre-P2b.
    core::Rectangle r = GravityHelper::Apply(Gravity::Fill, 400.0f, 300.0f, 120.0f, 90.0f);
    CHECK(r.x == doctest::Approx(0.0f));
    CHECK(r.y == doctest::Approx(0.0f));
    CHECK(r.width == doctest::Approx(400.0f));
    CHECK(r.height == doctest::Approx(300.0f));
}
