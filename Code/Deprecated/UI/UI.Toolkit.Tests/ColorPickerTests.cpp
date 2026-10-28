// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Smoke test for the toolkit ColorPicker: constructs, round-trips a color through HSV, sets an original.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
import foundation.ui.toolkit;

using namespace foundation::ui;
using namespace foundation::ui::toolkit;
using namespace foundation::core;
namespace core = foundation::core;

TEST_CASE("toolkit-colorpicker: ConstructAndRoundTrip")
{
    auto picker = core::MakeRef<ColorPicker>(core::DefaultAllocator());
    // 9 children: SV square, hue strip, alpha strip, 2 previews, hex, R/G/B.
    CHECK(picker->ChildCount() == 9u);

    // Default color is white (H=0,S=1,V=1 -> not white; the ctor seeds S=1,V=1,H=0 = red).
    const Color initial = picker->CurrentColor();
    CHECK(initial.a == doctest::Approx(1.0f));

    // Round-trip a known color through set/get.
    const Color target{0.25f, 0.5f, 0.75f, 1.0f};
    picker->SetColor(target);
    const Color got = picker->CurrentColor();
    CHECK(got.r == doctest::Approx(target.r).epsilon(0.01f));
    CHECK(got.g == doctest::Approx(target.g).epsilon(0.01f));
    CHECK(got.b == doctest::Approx(target.b).epsilon(0.01f));

    picker->SetOriginalColor(Color{1.0f, 0.0f, 0.0f, 1.0f});
}

TEST_CASE("toolkit-colorpicker: HSVMathIsInvertible")
{
    // Pure green.
    Color c = ColorPicker::HSVToRGB(120.0f, 1.0f, 1.0f, 1.0f);
    CHECK(c.r == doctest::Approx(0.0f));
    CHECK(c.g == doctest::Approx(1.0f));
    CHECK(c.b == doctest::Approx(0.0f));

    f32 h = 0, s = 0, v = 0;
    ColorPicker::RGBToHSV(0.0f, 1.0f, 0.0f, h, s, v);
    CHECK(h == doctest::Approx(120.0f));
    CHECK(s == doctest::Approx(1.0f));
    CHECK(v == doctest::Approx(1.0f));
}
