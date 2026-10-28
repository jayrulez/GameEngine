// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Smoke test for the toolkit HDRColorPicker: constructs, round-trips an HDR color, decomposes intensity.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
import foundation.ui.toolkit;

using namespace foundation::ui;
using namespace foundation::ui::toolkit;
using namespace foundation::core;
namespace core = foundation::core;

TEST_CASE("toolkit-hdrcolorpicker: ConstructAndRoundTrip")
{
    auto picker = core::MakeRef<HDRColorPicker>(core::DefaultAllocator());
    // 10 children: SV square, hue strip, alpha strip, 2 previews, intensity, R/G/B/A.
    CHECK(picker->ChildCount() == 10u);

    // An HDR color: green at intensity 4.
    const Float4 target{0.0f, 4.0f, 0.0f, 1.0f};
    picker->SetColor(target);
    const Float4 got = picker->CurrentColor();
    CHECK(got.x == doctest::Approx(0.0f));
    CHECK(got.y == doctest::Approx(4.0f).epsilon(0.01f));
    CHECK(got.z == doctest::Approx(0.0f));
    CHECK(got.w == doctest::Approx(1.0f));

    picker->SetOriginalColor(Float4{8.0f, 0.0f, 0.0f, 1.0f});
}

TEST_CASE("toolkit-hdrcolorpicker: IntensityDecomposition")
{
    // Vec4ToHSVI takes intensity = max channel; normalized color is RGB/intensity.
    f32 h = 0, s = 0, v = 0, i = 0, a = 0;
    HDRColorPicker::Vec4ToHSVI(Float4{2.0f, 0.0f, 0.0f, 0.5f}, h, s, v, i, a);
    CHECK(i == doctest::Approx(2.0f));
    CHECK(a == doctest::Approx(0.5f));
    CHECK(s == doctest::Approx(1.0f));

    // Recompose.
    const Float4 back = HDRColorPicker::HSVIToVec4(h, s, v, i, a);
    CHECK(back.x == doctest::Approx(2.0f).epsilon(0.01f));
    CHECK(back.w == doctest::Approx(0.5f));
}
